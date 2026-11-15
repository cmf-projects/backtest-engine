#include "population/order.hpp"
#include "population/population.hpp"
#include "population/price_ticks.hpp"
#include "population/tick.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using population::BookPatchKind;
using population::Level;
using population::TicksToPrice;
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

// Bids on the 0.01 grid, top tick first, no two prices shared.
std::vector<Level> BidLadder(long long top_ticks, int n, std::uint64_t qty) {
    std::vector<Level> levels;
    levels.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        levels.push_back(Level{TicksToPrice(top_ticks - i, 0.01), qty});
    }
    return levels;
}

}  // namespace

TEST(PopulationTick, FirstTickIsSharedKeyframe) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 200, {{100.0, 8}}, {{101.0, 5}}),
    });

    Population pop("C1", 0.01, std::move(tape),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1},
                    SlotConfig{.slot_id = 2, .md_lag_ns = 100, .delta_ns = 1}});
    pop.Start();

    const auto tick = pop.MakeTick();
    EXPECT_EQ(tick.pop_ts_ns, 100u);
    ASSERT_EQ(tick.books.size(), 1u);
    EXPECT_EQ(tick.books[0].component_id, "C1");
    EXPECT_EQ(tick.books[0].kind, BookPatchKind::Keyframe);
    EXPECT_EQ(tick.books[0].bids[0].qty, 10u);
    ASSERT_EQ(tick.slots.size(), 2u);
    EXPECT_EQ(tick.slots[0].n_books, 0u);
    EXPECT_EQ(tick.slots[1].n_books, 0u);
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
    ASSERT_EQ(tick.slots[0].n_books, 1u);
    EXPECT_EQ(tick.slots[0].books[0].kind, BookPatchKind::Diff);
    ASSERT_EQ(tick.slots[0].books[0].bids.size(), 1u);
    EXPECT_DOUBLE_EQ(tick.slots[0].books[0].bids[0].price, 100.0);
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
        ASSERT_EQ(tick.slots[0].n_books, 1u);
        EXPECT_EQ(tick.slots[0].books[0].kind, BookPatchKind::Diff);
        EXPECT_EQ(tick.slots[0].books[0].bids[0].qty, 8u);
        // slot2 view@100: still qty 10 → no patch
        EXPECT_EQ(tick.slots[1].n_books, 0u);
    }

    ASSERT_TRUE(pop.Advance());  // pop=300
    {
        const auto tick = pop.MakeTick();
        ASSERT_EQ(tick.slots.size(), 2u);
        EXPECT_EQ(tick.slots[0].books[0].bids[0].qty, 6u);  // view@300
        ASSERT_EQ(tick.slots[1].n_books, 1u);
        EXPECT_EQ(tick.slots[1].books[0].kind, BookPatchKind::Diff);
        EXPECT_EQ(tick.slots[1].books[0].bids[0].qty, 8u);  // view@200
    }
}

// TZ §8.4 / IMPL 4.2: lagged view = old file snap − *current* eaten (not historical).
TEST(PopulationTick, MdLagAppliesCurrentImpactToOldFileSnap) {
    const double lambda = 1e-15;  // negligible recovery over 100ns
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 10}}),
        Ev(2, 200, {{100.0, 10}}, {{101.0, 10}}),
    });

    Population pop("C1", 0.01, std::move(tape),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 100, .delta_ns = 1,
                               .impact_lambda = lambda}});
    pop.Start();
    (void)pop.MakeTick();  // pop=100, view clamps to floor ask=10 keyframe

    OrderBatch batch;
    batch.orders.push_back(NewOrder{
        .slot_id = 1,
        .client_order_id = 1,
        .component_id = "C1",
        .side = Side::Buy,
        .price = 101.0,
        .qty = 4,
        .tif = TimeInForce::Day,
    });
    EXPECT_EQ(pop.Submit(batch).order_results[0], OrderResult::Ok);

    ASSERT_TRUE(pop.Advance());  // pop=200 → fill @ ask; eaten=4
    const auto tick = pop.MakeTick();
    // view_ts=100: file ask still 10, current eaten 4 → effective 6
    ASSERT_EQ(tick.slots.size(), 1u);
    ASSERT_EQ(tick.slots[0].n_books, 1u);
    EXPECT_EQ(tick.slots[0].books[0].kind, BookPatchKind::Diff);
    ASSERT_FALSE(tick.slots[0].books[0].asks.empty());
    EXPECT_DOUBLE_EQ(tick.slots[0].books[0].asks[0].price, 101.0);
    EXPECT_EQ(tick.slots[0].books[0].asks[0].qty, 6u);
}

TEST(PopulationTick, FixedStepHoldsBookUntilDatasetEvent) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 1000, {{100.0, 8}}, {{101.0, 5}}),
    });
    Population pop("C1", 0.01, std::move(tape), OneSlot());
    pop.SetStepNs(300);
    pop.Start();
    (void)pop.MakeTick();

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 400u);
    EXPECT_EQ(pop.MakeTick().slots[0].n_books, 0u);

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 700u);
    EXPECT_EQ(pop.MakeTick().slots[0].n_books, 0u);

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 1000u);
    const auto tick = pop.MakeTick();
    ASSERT_EQ(tick.slots[0].n_books, 1u);
    EXPECT_EQ(tick.slots[0].books[0].kind, BookPatchKind::Diff);
    EXPECT_EQ(tick.slots[0].books[0].bids[0].qty, 8u);

    EXPECT_FALSE(pop.Advance());
}

TEST(PopulationTick, NegativeStepFollowsDatasetTimestamps) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 1000, {{100.0, 8}}, {{101.0, 5}}),
    });
    Population pop("C1", 0.01, std::move(tape), OneSlot());
    pop.SetStepNs(-1);
    pop.Start();
    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 1000u);
}

TEST(PopulationTick, LongDiffBecomesKeyframeOfFullBook) {
    // 40 bids shifted completely: 40 new prices + 40 deletions = 80 diff rows.
    constexpr long long kTop = 10'000;  // price 100.00
    constexpr int kDepth = 40;
    const std::vector<Level> asks = {{101.0, 5}};
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, BidLadder(kTop, kDepth, 10), asks),
        Ev(2, 200, BidLadder(kTop - kDepth, kDepth, 10), asks),
        Ev(3, 300, BidLadder(kTop - kDepth, kDepth, 10), asks),
    });

    Population pop("C1", 0.01, std::move(tape), OneSlot());
    pop.Start();
    (void)pop.MakeTick();

    ASSERT_TRUE(pop.Advance());
    {
        const auto tick = pop.MakeTick();
        ASSERT_EQ(tick.slots.size(), 1u);
        ASSERT_EQ(tick.slots[0].n_books, 1u);
        const auto& book = tick.slots[0].books[0];
        EXPECT_EQ(book.kind, BookPatchKind::Keyframe);
        ASSERT_EQ(book.bids.size(), static_cast<std::size_t>(kDepth));
        EXPECT_DOUBLE_EQ(book.bids.front().price, TicksToPrice(kTop - kDepth, 0.01));
        EXPECT_DOUBLE_EQ(book.bids.back().price, TicksToPrice(kTop - 2 * kDepth + 1, 0.01));
        EXPECT_EQ(book.bids.front().qty, 10u);
        ASSERT_EQ(book.asks.size(), 1u);
        EXPECT_DOUBLE_EQ(book.asks[0].price, 101.0);
        EXPECT_EQ(book.asks[0].qty, 5u);
    }

    ASSERT_TRUE(pop.Advance());
    {
        const auto tick = pop.MakeTick();
        ASSERT_EQ(tick.slots.size(), 1u);
        EXPECT_EQ(tick.slots[0].n_books, 0u);
    }
}

TEST(PopulationTick, QuietTickKeepsPatchBuffers) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 200, {{100.0, 10}}, {{101.0, 5}}),
        Ev(3, 300, {{100.0, 8}}, {{101.0, 5}}),
        Ev(4, 400, {{100.0, 8}}, {{101.0, 5}}),
        Ev(5, 500, {{100.0, 7}}, {{101.0, 5}}),
    });
    Population pop("C1", 0.01, std::move(tape), OneSlot());
    pop.Start();
    (void)pop.MakeTick();

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.MakeTick().slots[0].n_books, 0u);

    ASSERT_TRUE(pop.Advance());
    const population::TickView& born = pop.MakeTick();
    ASSERT_EQ(born.slots[0].n_books, 1u);
    const std::size_t stored = born.slots[0].books.size();
    const std::size_t bid_cap = born.slots[0].books[0].bids.capacity();
    const std::size_t ask_cap = born.slots[0].books[0].asks.capacity();
    EXPECT_GE(bid_cap, population::kMaxDiffLevelsPerSide);
    EXPECT_GE(ask_cap, population::kMaxDiffLevelsPerSide);

    ASSERT_TRUE(pop.Advance());
    const population::TickView& quiet = pop.MakeTick();
    EXPECT_EQ(quiet.slots[0].n_books, 0u);
    EXPECT_EQ(quiet.slots[0].books.size(), stored);
    EXPECT_EQ(quiet.slots[0].books[0].bids.capacity(), bid_cap);
    EXPECT_EQ(quiet.slots[0].books[0].asks.capacity(), ask_cap);

    ASSERT_TRUE(pop.Advance());
    const population::TickView& again = pop.MakeTick();
    ASSERT_EQ(again.slots[0].n_books, 1u);
    EXPECT_EQ(again.slots[0].books.size(), stored);
    EXPECT_EQ(again.slots[0].books[0].bids.capacity(), bid_cap);
    EXPECT_EQ(again.slots[0].books[0].asks.capacity(), ask_cap);
    EXPECT_EQ(again.slots[0].books[0].kind, BookPatchKind::Diff);
    EXPECT_EQ(again.slots[0].books[0].bids[0].qty, 7u);
}
