#include "population/population.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

using population::ComponentSpec;
using population::Level;
using population::Population;
using population::SlotConfig;
using population::SnapshotEvent;
using population::TapePtr;
using population::test::FakeTape;

namespace {

SnapshotEvent Ev(std::string symbol, std::uint64_t seq, std::uint64_t ts_ns,
                 std::uint64_t bid_qty) {
    SnapshotEvent e;
    e.seq = seq;
    e.ts_ns = ts_ns;
    e.symbol = std::move(symbol);
    e.bids = {Level{100.0, bid_qty}};
    e.asks = {Level{101.0, 5}};
    return e;
}

TapePtr MakeTape(std::vector<SnapshotEvent> events) {
    return std::make_unique<FakeTape>(std::move(events));
}

std::vector<SlotConfig> OneSlot() {
    return {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = 0.0}};
}

}  // namespace

// spot: 100, 300
// fut:  100, 200, 400
// Start → pop_ts = 100; Advance → 200, 300, 400 (merge timeline).
TEST(PopulationBundle, StartAndAdvanceFollowMergedTimeline) {
    std::vector<ComponentSpec> comps;
    comps.push_back(ComponentSpec{
        .component_id = "fut",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("FUT", 1, 100, 10),
            Ev("FUT", 2, 200, 20),
            Ev("FUT", 3, 400, 40),
        }),
    });
    comps.push_back(ComponentSpec{
        .component_id = "spot",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("SPOT", 1, 100, 1),
            Ev("SPOT", 2, 300, 3),
        }),
    });

    Population pop(std::move(comps), OneSlot());
    pop.Start();

    EXPECT_EQ(pop.clock().pop_ts_ns(), 100u);
    EXPECT_EQ(pop.component_count(), 2u);
    ASSERT_NE(pop.FindComponent("fut"), nullptr);
    ASSERT_NE(pop.FindComponent("spot"), nullptr);
    EXPECT_EQ(pop.FindComponent("fut")->window().size(), 1u);
    EXPECT_EQ(pop.FindComponent("spot")->window().size(), 1u);
    EXPECT_EQ(pop.FindComponent("fut")->current().bids[0].qty, 10u);
    EXPECT_EQ(pop.FindComponent("spot")->current().bids[0].qty, 1u);

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 200u);
    // fut updated; spot keep-last — window grows only on update
    EXPECT_EQ(pop.FindComponent("fut")->window().size(), 2u);
    EXPECT_EQ(pop.FindComponent("spot")->window().size(), 1u);
    EXPECT_EQ(pop.FindComponent("fut")->current().bids[0].qty, 20u);
    EXPECT_EQ(pop.FindComponent("spot")->current().bids[0].qty, 1u);

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 300u);
    EXPECT_EQ(pop.FindComponent("spot")->window().size(), 2u);
    EXPECT_EQ(pop.FindComponent("spot")->current().bids[0].qty, 3u);

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 400u);
    EXPECT_EQ(pop.FindComponent("fut")->current().bids[0].qty, 40u);

    EXPECT_FALSE(pop.Advance());
    EXPECT_TRUE(pop.ended());
}

TEST(PopulationBundle, RejectsEmptyOrTooManyComponents) {
    EXPECT_THROW(Population(std::vector<ComponentSpec>{}, OneSlot()), std::invalid_argument);

    std::vector<ComponentSpec> four;
    for (int i = 0; i < 4; ++i) {
        four.push_back(ComponentSpec{
            .component_id = "c" + std::to_string(i),
            .tick_size = 0.01,
            .tape = MakeTape({Ev("X", 1, 100, 1)}),
        });
    }
    EXPECT_THROW(Population(std::move(four), OneSlot()), std::invalid_argument);
}

TEST(PopulationBundle, ImpactIsPerSlotAndComponent) {
    std::vector<ComponentSpec> comps;
    comps.push_back(ComponentSpec{
        .component_id = "fut",
        .tick_size = 0.01,
        .tape = MakeTape({Ev("FUT", 1, 100, 10)}),
    });
    comps.push_back(ComponentSpec{
        .component_id = "spot",
        .tick_size = 0.01,
        .tape = MakeTape({Ev("SPOT", 1, 100, 1)}),
    });

    Population pop(std::move(comps),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = 0.5}});
    pop.Start();

    ASSERT_NE(pop.SlotImpact(1, "fut"), nullptr);
    ASSERT_NE(pop.SlotImpact(1, "spot"), nullptr);
    EXPECT_EQ(pop.SlotImpact(1, "fut"), pop.SlotImpact(1, "fut"));
    EXPECT_NE(pop.SlotImpact(1, "fut"), pop.SlotImpact(1, "spot"));
    EXPECT_EQ(pop.SlotImpact(1, "missing"), nullptr);
}
