#include "population/population.hpp"
#include "population/tape_window.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using population::Level;
using population::Population;
using population::SlotConfig;
using population::SnapshotEvent;
using population::TapeWindow;
using population::test::FakeTape;

namespace {

SnapshotEvent Ev(std::uint64_t seq, std::uint64_t ts_ns, std::uint64_t bid_qty) {
    SnapshotEvent e;
    e.seq = seq;
    e.ts_ns = ts_ns;
    e.symbol = "TEST";
    e.bids = {Level{100.0, bid_qty}};
    e.asks = {Level{101.0, 5}};
    return e;
}

}  // namespace

TEST(TapeWindow, AtOrBeforeAndTrim) {
    TapeWindow w;
    w.Append(Ev(1, 100, 10));
    w.Append(Ev(2, 200, 8));
    w.Append(Ev(3, 300, 6));

    EXPECT_EQ(w.floor_ts_ns(), 100u);
    EXPECT_EQ(w.head_ts_ns(), 300u);
    EXPECT_EQ(w.size(), 3u);

    EXPECT_FALSE(w.AtOrBefore(50).has_value());
    ASSERT_TRUE(w.AtOrBefore(100).has_value());
    EXPECT_EQ(w.AtOrBefore(100)->bids[0].qty, 10u);
    EXPECT_EQ(w.AtOrBefore(250)->bids[0].qty, 8u);
    EXPECT_EQ(w.AtOrBefore(300)->bids[0].qty, 6u);
    EXPECT_EQ(w.AtOrBefore(999)->bids[0].qty, 6u);

    w.TrimBefore(200);
    EXPECT_EQ(w.floor_ts_ns(), 200u);
    EXPECT_EQ(w.size(), 2u);
    EXPECT_FALSE(w.AtOrBefore(100).has_value());
    EXPECT_EQ(w.AtOrBefore(200)->bids[0].qty, 8u);
}

TEST(FileBook, AppendsHistoryOnAdvance) {
    auto tape = std::make_unique<FakeTape>(std::vector<SnapshotEvent>{
        Ev(1, 100, 10),
        Ev(2, 200, 8),
        Ev(3, 300, 6),
    });

    Population pop("C1", 0.01, std::move(tape),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1}});
    pop.Start();
    EXPECT_EQ(pop.book().window().size(), 1u);
    EXPECT_EQ(pop.book().window().head_ts_ns(), 100u);

    ASSERT_TRUE(pop.Advance());
    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.book().window().size(), 3u);
    EXPECT_EQ(pop.book().window().AtOrBefore(200)->bids[0].qty, 8u);

    const auto tick = pop.MakeTick();
    EXPECT_EQ(tick.pop_ts_ns, 300u);
    ASSERT_EQ(tick.slots.size(), 1u);
    ASSERT_EQ(tick.slots[0].books.size(), 1u);
    EXPECT_EQ(tick.slots[0].books[0].kind, population::BookPatchKind::Keyframe);
    EXPECT_EQ(tick.slots[0].books[0].bids[0].qty, 6u);
}
