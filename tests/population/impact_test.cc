#include "population/match.hpp"
#include "population/order.hpp"
#include "population/population.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using population::Level;
using population::NewOrder;
using population::OrderBatch;
using population::OrderResult;
using population::Population;
using population::PriceToTicks;
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

TEST(Impact, SecondFillSameSlotSeesEatenAsk) {
    // Tiny λ → recovery over 100ns negligible.
    const double lambda = 1e-15;
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(100, 5),
        Ev(200, 5),
        Ev(300, 5),
    });
    Population pop("C1", 0.01, std::move(tape),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = lambda},
                    SlotConfig{.slot_id = 2, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = lambda}});
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch first;
    first.orders.push_back(NewOrder{1, 1, "C1", Side::Buy, 101.0, 5, TimeInForce::Day});
    ASSERT_EQ(pop.Submit(first).order_results[0], OrderResult::Ok);
    ASSERT_TRUE(pop.Advance());
    ASSERT_EQ(pop.MakeTick().slots[0].trades[0].qty, 5u);

    ASSERT_NE(pop.SlotImpact(1), nullptr);
    EXPECT_NEAR(pop.SlotImpact(1)->EatenAt(Side::Sell, PriceToTicks(101.0, 0.01)), 5.0, 1e-6);

    OrderBatch second;
    second.orders.push_back(NewOrder{1, 2, "C1", Side::Buy, 101.0, 5, TimeInForce::Day});
    second.orders.push_back(NewOrder{2, 1, "C1", Side::Buy, 101.0, 5, TimeInForce::Day});
    const auto submitted = pop.Submit(second);
    ASSERT_EQ(submitted.order_results[0], OrderResult::Ok);
    ASSERT_EQ(submitted.order_results[1], OrderResult::Ok);

    ASSERT_TRUE(pop.Advance());
    const auto tick = pop.MakeTick();
    EXPECT_TRUE(tick.slots[0].trades.empty());
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 1u);
    ASSERT_EQ(tick.slots[1].trades.size(), 1u);
    EXPECT_EQ(tick.slots[1].trades[0].qty, 5u);
}

TEST(Impact, LambdaZeroDoesNotRecordEaten) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(100, 5),
        Ev(200, 5),
    });
    Population pop("C1", 0.01, std::move(tape),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = 0.0}});
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch batch;
    batch.orders.push_back(NewOrder{1, 1, "C1", Side::Buy, 101.0, 5, TimeInForce::Day});
    ASSERT_EQ(pop.Submit(batch).order_results[0], OrderResult::Ok);
    ASSERT_TRUE(pop.Advance());
    ASSERT_EQ(pop.MakeTick().slots[0].trades[0].qty, 5u);
    EXPECT_EQ(pop.SlotImpact(1)->EatenAt(Side::Sell, PriceToTicks(101.0, 0.01)), 0.0);
}
