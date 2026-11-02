#include "population/order.hpp"
#include "population/population.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using population::CancelOrder;
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
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);

    OrderBatch missing;
    missing.cancels.push_back(CancelOrder{.slot_id = 1, .client_order_id = 1});
    EXPECT_EQ(pop.Submit(missing).cancel_results[0], OrderResult::NoSuchOrder);
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
}
