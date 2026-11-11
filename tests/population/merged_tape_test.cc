#include "population/merged_tape.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using population::ComponentSnap;
using population::Level;
using population::MergedLeg;
using population::MergedStep;
using population::MergedTape;
using population::SnapshotEvent;
using population::TapePtr;
using population::test::FakeTape;

namespace {

SnapshotEvent Ev(std::string symbol, std::uint64_t seq, std::uint64_t ts_ns, std::uint64_t bid_qty) {
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

const ComponentSnap* Find(const MergedStep& step, const std::string& id) {
    for (const auto& c : step.components) {
        if (c.component_id == id) {
            return &c;
        }
    }
    return nullptr;
}

}  // namespace

// Two tapes with staggered timestamps (gaps): keep-last fills missing leg.
//
//   spot: 100,      300
//   fut:  100, 200,      400
TEST(MergedTape, TwoTapesWithGapsKeepLast) {
    std::vector<MergedLeg> legs;
    legs.push_back(MergedLeg{.component_id = "fut",
                             .tape = MakeTape({
                                 Ev("FUT", 1, 100, 10),
                                 Ev("FUT", 2, 200, 20),
                                 Ev("FUT", 3, 400, 40),
                             })});
    legs.push_back(MergedLeg{.component_id = "spot",
                             .tape = MakeTape({
                                 Ev("SPOT", 1, 100, 1),
                                 Ev("SPOT", 2, 300, 3),
                             })});
    MergedTape merge(std::move(legs));

    EXPECT_EQ(merge.StartTs(), 100u);
    EXPECT_EQ(merge.leg_count(), 2u);

    {
        const auto step = merge.Next();
        ASSERT_TRUE(step.has_value());
        EXPECT_EQ(step->ts_ns, 100u);
        ASSERT_EQ(step->components.size(), 2u);
        // Sorted by component_id: fut, spot
        EXPECT_EQ(step->components[0].component_id, "fut");
        EXPECT_EQ(step->components[1].component_id, "spot");
        EXPECT_TRUE(Find(*step, "spot")->updated);
        EXPECT_TRUE(Find(*step, "fut")->updated);
        EXPECT_EQ(Find(*step, "spot")->snap.bids[0].qty, 1u);
        EXPECT_EQ(Find(*step, "fut")->snap.bids[0].qty, 10u);
    }

    {
        // Only fut at 200; spot keep-last from 100.
        const auto step = merge.Next();
        ASSERT_TRUE(step.has_value());
        EXPECT_EQ(step->ts_ns, 200u);
        ASSERT_EQ(step->components.size(), 2u);
        EXPECT_FALSE(Find(*step, "spot")->updated);
        EXPECT_TRUE(Find(*step, "fut")->updated);
        EXPECT_EQ(Find(*step, "spot")->snap.bids[0].qty, 1u);
        EXPECT_EQ(Find(*step, "fut")->snap.bids[0].qty, 20u);
        EXPECT_EQ(Find(*step, "spot")->snap.ts_ns, 100u);
        EXPECT_EQ(Find(*step, "fut")->snap.ts_ns, 200u);
    }

    {
        // Only spot at 300; fut keep-last from 200.
        const auto step = merge.Next();
        ASSERT_TRUE(step.has_value());
        EXPECT_EQ(step->ts_ns, 300u);
        EXPECT_TRUE(Find(*step, "spot")->updated);
        EXPECT_FALSE(Find(*step, "fut")->updated);
        EXPECT_EQ(Find(*step, "spot")->snap.bids[0].qty, 3u);
        EXPECT_EQ(Find(*step, "fut")->snap.bids[0].qty, 20u);
    }

    {
        // Only fut at 400; spot keep-last from 300.
        const auto step = merge.Next();
        ASSERT_TRUE(step.has_value());
        EXPECT_EQ(step->ts_ns, 400u);
        EXPECT_FALSE(Find(*step, "spot")->updated);
        EXPECT_TRUE(Find(*step, "fut")->updated);
        EXPECT_EQ(Find(*step, "spot")->snap.bids[0].qty, 3u);
        EXPECT_EQ(Find(*step, "fut")->snap.bids[0].qty, 40u);
    }

    EXPECT_FALSE(merge.Next().has_value());
}

TEST(MergedTape, LateStartingLegOmittedUntilFirstEvent) {
    // fut starts later than spot — first step has only spot.
    std::vector<MergedLeg> legs;
    legs.push_back(MergedLeg{.component_id = "fut",
                             .tape = MakeTape({
                                 Ev("FUT", 1, 200, 20),
                             })});
    legs.push_back(MergedLeg{.component_id = "spot",
                             .tape = MakeTape({
                                 Ev("SPOT", 1, 100, 1),
                                 Ev("SPOT", 2, 200, 2),
                             })});
    MergedTape merge(std::move(legs));

    {
        const auto step = merge.Next();
        ASSERT_TRUE(step.has_value());
        EXPECT_EQ(step->ts_ns, 100u);
        ASSERT_EQ(step->components.size(), 1u);
        EXPECT_EQ(step->components[0].component_id, "spot");
        EXPECT_TRUE(step->components[0].updated);
    }

    {
        const auto step = merge.Next();
        ASSERT_TRUE(step.has_value());
        EXPECT_EQ(step->ts_ns, 200u);
        ASSERT_EQ(step->components.size(), 2u);
        EXPECT_TRUE(Find(*step, "spot")->updated);
        EXPECT_TRUE(Find(*step, "fut")->updated);
    }

    EXPECT_FALSE(merge.Next().has_value());
}

TEST(MergedTape, RejectsDuplicateComponentId) {
    std::vector<MergedLeg> legs;
    legs.push_back(MergedLeg{.component_id = "spot", .tape = MakeTape({Ev("A", 1, 100, 1)})});
    legs.push_back(MergedLeg{.component_id = "spot", .tape = MakeTape({Ev("B", 1, 100, 1)})});
    EXPECT_THROW(MergedTape(std::move(legs)), std::invalid_argument);
}

TEST(MergedTape, SingleLegIsIdentity) {
    std::vector<MergedLeg> legs;
    legs.push_back(MergedLeg{.component_id = "spot",
                             .tape = MakeTape({
                                 Ev("SPOT", 1, 100, 1),
                                 Ev("SPOT", 2, 200, 2),
                             })});
    MergedTape merge(std::move(legs));

    auto a = merge.Next();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->ts_ns, 100u);
    ASSERT_EQ(a->components.size(), 1u);
    EXPECT_TRUE(a->components[0].updated);

    auto b = merge.Next();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->ts_ns, 200u);
    EXPECT_TRUE(b->components[0].updated);

    EXPECT_FALSE(merge.Next().has_value());
}
