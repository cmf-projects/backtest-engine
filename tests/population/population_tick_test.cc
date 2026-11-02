#include "population/population.hpp"
#include "population/tick.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using population::BookPatchKind;
using population::Level;
using population::Population;
using population::SlotConfig;
using population::SnapshotEvent;
using population::test::FakeTape;

namespace {

SnapshotEvent Ev(std::uint64_t seq, std::uint64_t ts_ns, std::vector<Level> bids,
                 std::vector<Level> asks) {
    SnapshotEvent e;
    e.seq = seq;
    e.ts_ns = ts_ns;
    e.symbol = "TEST";
    e.bids = std::move(bids);
    e.asks = std::move(asks);
    return e;
}

std::vector<SlotConfig> OneSlot() {
    return {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = 0.0}};
}

}  // namespace

TEST(PopulationTick, FirstTickPerSlotKeyframe) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 200, {{100.0, 8}}, {{101.0, 5}}),
    });

    Population pop("C1", 0.01, std::move(tape), OneSlot());
    pop.Start();

    const auto tick = pop.MakeTick();
    EXPECT_EQ(tick.pop_ts_ns, 100u);
    EXPECT_TRUE(tick.books.empty());
    ASSERT_EQ(tick.slots.size(), 1u);
    EXPECT_EQ(tick.slots[0].slot_id, 1u);
    ASSERT_EQ(tick.slots[0].books.size(), 1u);
    EXPECT_EQ(tick.slots[0].books[0].kind, BookPatchKind::Keyframe);
    EXPECT_EQ(tick.slots[0].books[0].bids[0].qty, 10u);
}

TEST(PopulationTick, LaterTickSendsPerSlotDiff) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 200, {{100.0, 8}}, {{101.0, 5}}),
    });

    Population pop("C1", 0.01, std::move(tape), OneSlot());
    pop.Start();
    (void)pop.MakeTick();

    ASSERT_TRUE(pop.Advance());
    const auto tick = pop.MakeTick();

    EXPECT_EQ(tick.pop_ts_ns, 200u);
    EXPECT_TRUE(tick.books.empty());
    ASSERT_EQ(tick.slots.size(), 1u);
    ASSERT_EQ(tick.slots[0].books.size(), 1u);
    EXPECT_EQ(tick.slots[0].books[0].kind, BookPatchKind::Diff);
    ASSERT_EQ(tick.slots[0].books[0].bids.size(), 1u);
    EXPECT_DOUBLE_EQ(tick.slots[0].books[0].bids[0].price, 100.0);
    EXPECT_EQ(tick.slots[0].books[0].bids[0].qty, 8u);
}

TEST(PopulationTick, QueueResyncSendsKeyframeNotDiff) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 200, {{100.0, 8}}, {{101.0, 5}}),
    });

    Population pop("C1", 0.01, std::move(tape), OneSlot());
    pop.Start();
    (void)pop.MakeTick();
    ASSERT_TRUE(pop.Advance());

    pop.QueueResync(1);
    const auto tick = pop.MakeTick();

    ASSERT_EQ(tick.slots.size(), 1u);
    ASSERT_EQ(tick.slots[0].books.size(), 1u);
    EXPECT_EQ(tick.slots[0].books[0].kind, BookPatchKind::Keyframe);
    ASSERT_EQ(tick.slots[0].books[0].bids.size(), 1u);
    EXPECT_EQ(tick.slots[0].books[0].bids[0].qty, 8u);
}

TEST(PopulationTick, MdLagSlotsSeeDifferentBooks) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 200, {{100.0, 8}}, {{101.0, 5}}),
        Ev(3, 300, {{100.0, 6}}, {{101.0, 5}}),
    });

    std::vector<SlotConfig> slots = {
        SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1},
        SlotConfig{.slot_id = 2, .md_lag_ns = 100, .delta_ns = 1},
    };
    Population pop("C1", 0.01, std::move(tape), std::move(slots));
    pop.Start();
    (void)pop.MakeTick();  // both keyframe @100 (lag slot clamps to floor)

    ASSERT_TRUE(pop.Advance());  // pop=200
    {
        const auto tick = pop.MakeTick();
        ASSERT_EQ(tick.slots.size(), 2u);
        // slot1 view@200: qty 8 → Diff
        ASSERT_EQ(tick.slots[0].books.size(), 1u);
        EXPECT_EQ(tick.slots[0].books[0].kind, BookPatchKind::Diff);
        EXPECT_EQ(tick.slots[0].books[0].bids[0].qty, 8u);
        // slot2 view@100: still qty 10 → no patch
        EXPECT_TRUE(tick.slots[1].books.empty());
    }

    ASSERT_TRUE(pop.Advance());  // pop=300
    {
        const auto tick = pop.MakeTick();
        ASSERT_EQ(tick.slots.size(), 2u);
        EXPECT_EQ(tick.slots[0].books[0].bids[0].qty, 6u);  // view@300
        ASSERT_EQ(tick.slots[1].books.size(), 1u);
        EXPECT_EQ(tick.slots[1].books[0].kind, BookPatchKind::Diff);
        EXPECT_EQ(tick.slots[1].books[0].bids[0].qty, 8u);  // view@200
    }
}
