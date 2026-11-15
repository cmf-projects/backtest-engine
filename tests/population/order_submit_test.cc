#include "population/order.hpp"
#include "population/population.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

using population::CancelOrder;
using population::ExecType;
using population::Level;
using population::NewOrder;
using population::OrderBatch;
using population::OrderResult;
using population::Population;
using population::Side;
using population::SlotConfig;
using population::SnapshotEvent;
using population::TimeInForce;
using population::test::FakeTape;

namespace {

SnapshotEvent Ev(std::uint64_t ts_ns) {
    SnapshotEvent e;
    e.seq = 1;
    e.ts_ns = ts_ns;
    e.symbol = "TEST";
    e.bids = {Level{100.0, 10}};
    e.asks = {Level{101.0, 5}};
    return e;
}

Population MakePop() {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(100),
        Ev(200),
    });
    return Population("C1", 0.01, std::move(tape),
                      {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 50}});
}

NewOrder Buy(std::uint64_t cl_id, double px, std::uint64_t qty) {
    return NewOrder{
        .slot_id = 1,
        .client_order_id = cl_id,
        .component_id = "C1",
        .side = Side::Buy,
        .price = px,
        .qty = qty,
        .tif = TimeInForce::Day,
    };
}

}  // namespace

TEST(OrderSubmit, AcceptSetsEffectiveTsToPopPlusDelta) {
    Population pop = MakePop();
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch batch;
    batch.orders.push_back(Buy(7, 100.0, 3));
    const auto result = pop.Submit(batch);

    ASSERT_EQ(result.order_results.size(), 1u);
    EXPECT_EQ(result.order_results[0], OrderResult::Ok);

    const auto* book = pop.SlotBook(1);
    ASSERT_NE(book, nullptr);
    ASSERT_EQ(book->LiveCount(), 1u);
    EXPECT_EQ(book->orders()[0].client_order_id, 7u);
    EXPECT_EQ(book->orders()[0].effective_ts_ns, 150u);  // 100 + Δ50
    EXPECT_EQ(book->orders()[0].leaves, 3u);
}

TEST(OrderSubmit, DuplicateAndCancel) {
    Population pop = MakePop();
    pop.Start();

    OrderBatch first;
    first.orders.push_back(Buy(1, 100.0, 2));
    EXPECT_EQ(pop.Submit(first).order_results[0], OrderResult::Ok);

    OrderBatch dup;
    dup.orders.push_back(Buy(1, 100.0, 1));
    EXPECT_EQ(pop.Submit(dup).order_results[0], OrderResult::DuplicateOrderId);

    OrderBatch cancel;
    cancel.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 1});
    EXPECT_EQ(pop.Submit(cancel).cancel_results[0], OrderResult::Ok);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 1u);
    EXPECT_EQ(pop.SlotBook(1)->orders()[0].cancel_effective_ts_ns, 150u);

    OrderBatch missing;
    missing.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 1});
    EXPECT_EQ(pop.Submit(missing).cancel_results[0], OrderResult::NoSuchOrder);

    ASSERT_TRUE(pop.Advance());
    const auto tick = pop.MakeTick();
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);
    ASSERT_EQ(tick.slots[0].trades.size(), 1u);
    EXPECT_EQ(tick.slots[0].trades[0].exec_type, ExecType::Cancel);
}

TEST(OrderSubmit, CancelOfSameBatchOrderIsRejected) {
    Population pop = MakePop();
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch batch;
    batch.orders.push_back(Buy(7, 100.0, 3));
    batch.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 7});
    const auto result = pop.Submit(batch);
    ASSERT_EQ(result.order_results.size(), 1u);
    EXPECT_EQ(result.order_results[0], OrderResult::Ok);
    ASSERT_EQ(result.cancel_results.size(), 1u);
    EXPECT_EQ(result.cancel_results[0], OrderResult::NoSuchOrder);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);

    OrderBatch again;
    again.orders.push_back(Buy(7, 100.0, 3));
    EXPECT_EQ(pop.Submit(again).order_results[0], OrderResult::Ok);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 1u);

    OrderBatch next_tick;
    next_tick.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 7});
    EXPECT_EQ(pop.Submit(next_tick).cancel_results[0], OrderResult::Ok);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 1u);
    EXPECT_EQ(pop.SlotBook(1)->orders()[0].cancel_effective_ts_ns, 150u);
}

TEST(OrderSubmit, RejectsBadComponentQtyGridSlot) {
    Population pop = MakePop();
    pop.Start();

    OrderBatch batch;
    batch.orders.push_back(Buy(1, 100.0, 0));
    batch.orders.push_back(Buy(2, 100.005, 1));  // off 0.01 grid
    NewOrder bad_comp = Buy(3, 100.0, 1);
    bad_comp.component_id = "OTHER";
    batch.orders.push_back(bad_comp);
    NewOrder bad_slot = Buy(4, 100.0, 1);
    bad_slot.slot_id = 99;
    batch.orders.push_back(bad_slot);

    const auto result = pop.Submit(batch);
    ASSERT_EQ(result.order_results.size(), 4u);
    EXPECT_EQ(result.order_results[0], OrderResult::InvalidQty);
    EXPECT_EQ(result.order_results[1], OrderResult::PriceOffGrid);
    EXPECT_EQ(result.order_results[2], OrderResult::UnknownComponent);
    EXPECT_EQ(result.order_results[3], OrderResult::UnknownSlot);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);  // all-or-nothing: nothing applied
}

TEST(OrderSubmit, MixedBatchAppliesNothingOnOneBadLine) {
    Population pop = MakePop();
    pop.Start();

    OrderBatch batch;
    batch.orders.push_back(Buy(1, 100.0, 2));
    batch.orders.push_back(Buy(2, 100.0, 0));  // bad qty
    const auto result = pop.Submit(batch);
    EXPECT_EQ(result.order_results[0], OrderResult::Ok);
    EXPECT_EQ(result.order_results[1], OrderResult::InvalidQty);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);

    const auto failures = result.Failures(batch);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0].client_order_id, 2u);
    EXPECT_EQ(failures[0].code, OrderResult::InvalidQty);
}

TEST(OrderSubmit, HugeClientOrderIdIsAccepted) {
    Population pop = MakePop();
    pop.Start();
    OrderBatch batch;
    batch.orders.push_back(Buy(50'000'000, 100.0, 1));
    EXPECT_EQ(pop.Submit(batch).order_results[0], OrderResult::Ok);
    ASSERT_EQ(pop.SlotBook(1)->LiveCount(), 1u);
    EXPECT_EQ(pop.SlotBook(1)->orders()[0].client_order_id, 50'000'000u);

    Population pop_max = MakePop();
    pop_max.Start();
    OrderBatch huge;
    huge.orders.push_back(Buy(std::numeric_limits<std::uint64_t>::max(), 100.0, 1));
    EXPECT_EQ(pop_max.Submit(huge).order_results[0], OrderResult::Ok);
    EXPECT_EQ(pop_max.SlotBook(1)->LiveCount(), 1u);
}

TEST(OrderSubmit, ReuseAfterCancelIsRejected) {
    Population pop = MakePop();
    pop.Start();

    OrderBatch first;
    first.orders.push_back(Buy(1, 100.0, 1));
    EXPECT_EQ(pop.Submit(first).order_results[0], OrderResult::Ok);

    OrderBatch cancel;
    cancel.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 1});
    EXPECT_EQ(pop.Submit(cancel).cancel_results[0], OrderResult::Ok);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 1u);

    OrderBatch again;
    again.orders.push_back(Buy(1, 100.0, 1));
    EXPECT_EQ(pop.Submit(again).order_results[0], OrderResult::DuplicateOrderId);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 1u);
}

TEST(OrderSubmit, ReuseAfterFillIsRejected) {
    Population pop = MakePop();
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch batch;
    batch.orders.push_back(Buy(11, 101.0, 1));
    EXPECT_EQ(pop.Submit(batch).order_results[0], OrderResult::Ok);

    ASSERT_TRUE(pop.Advance());
    const auto tick = pop.MakeTick();
    ASSERT_EQ(tick.slots[0].trades.size(), 1u);
    EXPECT_EQ(tick.slots[0].trades[0].leaves, 0u);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);

    OrderBatch again;
    again.orders.push_back(Buy(11, 101.0, 1));
    EXPECT_EQ(pop.Submit(again).order_results[0], OrderResult::DuplicateOrderId);
}

TEST(OrderSubmit, DecreasingIdsInBatchRejected) {
    Population pop = MakePop();
    pop.Start();

    OrderBatch batch;
    batch.orders.push_back(Buy(2, 100.0, 1));
    batch.orders.push_back(Buy(1, 100.0, 1));
    const auto result = pop.Submit(batch);
    ASSERT_EQ(result.order_results.size(), 2u);
    EXPECT_EQ(result.order_results[0], OrderResult::Ok);
    EXPECT_EQ(result.order_results[1], OrderResult::DuplicateOrderId);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);

    // Rejected batch does not move the watermark, so id 1 can still be sent.
    OrderBatch later;
    later.orders.push_back(Buy(1, 100.0, 1));
    EXPECT_EQ(pop.Submit(later).order_results[0], OrderResult::Ok);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 1u);
}

TEST(OrderSubmit, DuplicateCancelInBatchIsRejected) {
    Population pop = MakePop();
    pop.Start();

    OrderBatch first;
    first.orders.push_back(Buy(4, 100.0, 1));
    EXPECT_EQ(pop.Submit(first).order_results[0], OrderResult::Ok);

    OrderBatch twice;
    twice.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 4});
    twice.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 4});
    const auto result = pop.Submit(twice);
    ASSERT_EQ(result.cancel_results.size(), 2u);
    EXPECT_EQ(result.cancel_results[0], OrderResult::Ok);
    EXPECT_EQ(result.cancel_results[1], OrderResult::NoSuchOrder);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 1u);

    OrderBatch once;
    once.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 4});
    EXPECT_EQ(pop.Submit(once).cancel_results[0], OrderResult::Ok);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 1u);
    EXPECT_EQ(pop.SlotBook(1)->orders()[0].cancel_effective_ts_ns, 150u);
}

SnapshotEvent EvAsk(std::uint64_t ts_ns, double ask) {
    SnapshotEvent e;
    e.seq = ts_ns;
    e.ts_ns = ts_ns;
    e.symbol = "TEST";
    e.bids = {Level{100.0, 10}};
    e.asks = {Level{ask, 5}};
    return e;
}

Population MakeAskPop(std::vector<SnapshotEvent> events) {
    return Population("C1", 0.01, std::make_unique<FakeTape>(std::move(events)),
                      {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 50}});
}

TEST(OrderSubmit, BookFillsWhileCancelIsStillInFlight) {
    Population pop = MakeAskPop({
        EvAsk(100, 105.0),
        EvAsk(160, 105.0),
        EvAsk(200, 101.0),
    });
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch order;
    order.orders.push_back(Buy(1, 101.0, 1));
    ASSERT_EQ(pop.Submit(order).order_results[0], OrderResult::Ok);

    ASSERT_TRUE(pop.Advance());  // 160, ask still 105
    EXPECT_TRUE(pop.MakeTick().slots[0].trades.empty());

    OrderBatch cancel;
    cancel.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 1});
    ASSERT_EQ(pop.Submit(cancel).cancel_results[0], OrderResult::Ok);
    EXPECT_EQ(pop.SlotBook(1)->orders()[0].cancel_effective_ts_ns, 210u);

    ASSERT_TRUE(pop.Advance());  // 200 < 210, ask crosses 101
    const auto tick = pop.MakeTick();
    ASSERT_EQ(tick.slots[0].trades.size(), 1u);
    EXPECT_EQ(tick.slots[0].trades[0].exec_type, ExecType::Fill);
    EXPECT_EQ(tick.slots[0].trades[0].qty, 1u);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);
}

TEST(OrderSubmit, CancelPastDueDoesNotFill) {
    Population pop = MakeAskPop({
        EvAsk(100, 105.0),
        EvAsk(160, 105.0),
        EvAsk(250, 101.0),
    });
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch order;
    order.orders.push_back(Buy(1, 101.0, 1));
    ASSERT_EQ(pop.Submit(order).order_results[0], OrderResult::Ok);

    ASSERT_TRUE(pop.Advance());  // 160
    EXPECT_TRUE(pop.MakeTick().slots[0].trades.empty());

    OrderBatch cancel;
    cancel.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 1});
    ASSERT_EQ(pop.Submit(cancel).cancel_results[0], OrderResult::Ok);

    ASSERT_TRUE(pop.Advance());  // 250 > 210
    const auto tick = pop.MakeTick();
    ASSERT_EQ(tick.slots[0].trades.size(), 1u);
    EXPECT_EQ(tick.slots[0].trades[0].exec_type, ExecType::Cancel);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);
}
