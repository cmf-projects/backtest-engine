#include "population/order.hpp"
#include "population/population.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

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

SnapshotEvent Ev(std::uint64_t ts_ns, std::uint64_t ask_qty) {
    SnapshotEvent e;
    e.seq = ts_ns;
    e.ts_ns = ts_ns;
    e.symbol = "TEST";
    e.bids = {Level{100.0, 10}};
    e.asks = {Level{101.0, ask_qty}};
    return e;
}

}  // namespace

TEST(MatchDue, BuyFillsAfterDeltaAgainstAsk) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(100, 5),
        Ev(200, 5),
    });
    Population pop("C1", 0.01, std::move(tape),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 50}});
    pop.Start();
    (void)pop.MakeTick();  // pop=100, no orders yet

    OrderBatch batch;
    batch.orders.push_back(NewOrder{
        .slot_id = 1,
        .client_order_id = 42,
        .component_id = "C1",
        .side = Side::Buy,
        .price = 101.0,
        .qty = 3,
        .tif = TimeInForce::Day,
    });
    EXPECT_EQ(pop.Submit(batch).order_results[0], OrderResult::Ok);
    // effective_ts = 150; still before next MakeTick at 100 — no second match same ts
    // without Advance. Advance to 200 then match.

    ASSERT_TRUE(pop.Advance());
    const auto tick = pop.MakeTick();
    ASSERT_EQ(tick.slots.size(), 1u);
    ASSERT_EQ(tick.slots[0].trades.size(), 1u);
    EXPECT_EQ(tick.slots[0].trades[0].exec_type, ExecType::Fill);
    EXPECT_EQ(tick.slots[0].trades[0].client_order_id, 42u);
    EXPECT_EQ(tick.slots[0].trades[0].qty, 3u);
    EXPECT_EQ(tick.slots[0].trades[0].leaves, 0u);
    EXPECT_DOUBLE_EQ(tick.slots[0].trades[0].price, 101.0);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);
}

TEST(MatchDue, SlotsIsolatedSameFileLiquidity) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(100, 5),
        Ev(200, 5),
    });
    Population pop("C1", 0.01, std::move(tape),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1},
                    SlotConfig{.slot_id = 2, .md_lag_ns = 0, .delta_ns = 1}});
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch batch;
    batch.orders.push_back(NewOrder{1, 1, "C1", Side::Buy, 101.0, 5, TimeInForce::Day});
    batch.orders.push_back(NewOrder{2, 1, "C1", Side::Buy, 101.0, 5, TimeInForce::Day});
    const auto submitted = pop.Submit(batch);
    EXPECT_EQ(submitted.order_results[0], OrderResult::Ok);
    EXPECT_EQ(submitted.order_results[1], OrderResult::Ok);

    ASSERT_TRUE(pop.Advance());
    const auto tick = pop.MakeTick();
    ASSERT_EQ(tick.slots.size(), 2u);
    ASSERT_EQ(tick.slots[0].trades.size(), 1u);
    ASSERT_EQ(tick.slots[1].trades.size(), 1u);
    EXPECT_EQ(tick.slots[0].trades[0].qty, 5u);
    EXPECT_EQ(tick.slots[1].trades[0].qty, 5u);  // same file ask, independent copies
}

TEST(MatchDue, IocCancelsLeftover) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(100, 2),
        Ev(200, 2),
    });
    Population pop("C1", 0.01, std::move(tape),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1}});
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch batch;
    batch.orders.push_back(
        NewOrder{1, 9, "C1", Side::Buy, 101.0, 5, TimeInForce::Ioc});
    EXPECT_EQ(pop.Submit(batch).order_results[0], OrderResult::Ok);

    ASSERT_TRUE(pop.Advance());
    const auto tick = pop.MakeTick();
    ASSERT_EQ(tick.slots[0].trades.size(), 2u);
    EXPECT_EQ(tick.slots[0].trades[0].exec_type, ExecType::Fill);
    EXPECT_EQ(tick.slots[0].trades[0].qty, 2u);
    EXPECT_EQ(tick.slots[0].trades[0].leaves, 3u);
    EXPECT_EQ(tick.slots[0].trades[1].exec_type, ExecType::Cancel);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);
}
