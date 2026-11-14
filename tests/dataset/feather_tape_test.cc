#include "dataset/feather_tape.hpp"
#include "dataset/make_tape.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Fixture: scripts/gen_feather_fixture.py → testdata/dataset/bbo_1m
namespace {

using dataset::FeatherBboTape;
using population::SnapshotEvent;

const std::string kRoot = std::string(CMF_TESTDATA_DIR) + "/dataset/bbo_1m";
const std::string kDay1 = kRoot + "/20250101/GLBX-TEST";  // LZ4
const std::string kDay3 = kRoot + "/20250103/GLBX-TEST";  // uncompressed

constexpr std::uint64_t kMinute = 60'000'000'000ULL;
constexpr std::uint64_t kDay = 86'400'000'000'000ULL;
constexpr std::uint64_t kT0 = 1'735'689'600'000'000'000ULL;

std::vector<SnapshotEvent> Drain(population::Tape& tape) {
    std::vector<SnapshotEvent> out;
    while (auto ev = tape.Next()) {
        out.push_back(std::move(*ev));
    }
    return out;
}

TEST(FeatherBboTape, OnePartKeepsOnlySymbol) {
    FeatherBboTape tape(kDay1 + "/part-00000.feather", "MGCZ5");
    EXPECT_EQ(tape.StartTs(), kT0 + kMinute);
    EXPECT_EQ(tape.FirstSymbol(), "MGCZ5");

    const auto events = Drain(tape);
    ASSERT_EQ(events.size(), 2u);

    EXPECT_EQ(events[0].seq, 1u);
    EXPECT_EQ(events[0].ts_ns, kT0 + kMinute);
    EXPECT_EQ(events[0].symbol, "MGCZ5");
    ASSERT_EQ(events[0].bids.size(), 1u);
    ASSERT_EQ(events[0].asks.size(), 1u);
    EXPECT_DOUBLE_EQ(events[0].bids[0].price, 4230.1);
    EXPECT_EQ(events[0].bids[0].qty, 3u);
    EXPECT_DOUBLE_EQ(events[0].asks[0].price, 4230.3);
    EXPECT_EQ(events[0].asks[0].qty, 1u);

    // UNDEF ask → empty ask side.
    EXPECT_EQ(events[1].seq, 2u);
    EXPECT_EQ(events[1].ts_ns, kT0 + 2 * kMinute);
    ASSERT_EQ(events[1].bids.size(), 1u);
    EXPECT_DOUBLE_EQ(events[1].bids[0].price, 4230.2);
    EXPECT_TRUE(events[1].asks.empty());
}

TEST(FeatherBboTape, ReadsUncompressedPart) {
    FeatherBboTape tape(kDay3 + "/part-00000.feather", "MGCZ5");
    const auto events = Drain(tape);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].ts_ns, kT0 + 2 * kDay + kMinute);
    ASSERT_EQ(events[0].asks.size(), 1u);
    EXPECT_DOUBLE_EQ(events[0].asks[0].price, 4240.0);
}

TEST(FeatherBboTape, NegativeSpreadPrices) {
    FeatherBboTape tape(kDay1, "MGCZ5-MGCG6");
    const auto events = Drain(tape);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_DOUBLE_EQ(events[0].bids[0].price, -12.3);
    EXPECT_DOUBLE_EQ(events[0].asks[0].price, -12.1);
}

TEST(FeatherBboTape, RootReplaysDaysInOrderAndSkipsMissing) {
    FeatherBboTape tape(kRoot, "MGCZ5");
    const auto events = Drain(tape);
    // Day 2 has no MGCZ5; day 3 maps it to another instrument_id.
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[2].seq, 3u);
    EXPECT_EQ(events[2].ts_ns, kT0 + 2 * kDay + kMinute);
    EXPECT_TRUE(events[2].bids.empty());
    ASSERT_EQ(events[2].asks.size(), 1u);
    EXPECT_DOUBLE_EQ(events[2].asks[0].price, 4240.0);
    EXPECT_EQ(events[2].asks[0].qty, 2u);

    for (std::size_t i = 1; i < events.size(); ++i) {
        EXPECT_LT(events[i - 1].ts_ns, events[i].ts_ns);
    }
}

TEST(FeatherBboTape, NextIntoMatchesNext) {
    FeatherBboTape a(kRoot, "SILZ5");
    FeatherBboTape b(kRoot, "SILZ5");
    SnapshotEvent ev;
    std::size_t n = 0;
    while (a.NextInto(ev)) {
        const auto other = b.Next();
        ASSERT_TRUE(other.has_value());
        EXPECT_EQ(ev.seq, other->seq);
        EXPECT_EQ(ev.ts_ns, other->ts_ns);
        ASSERT_EQ(ev.bids.size(), other->bids.size());
        EXPECT_DOUBLE_EQ(ev.bids[0].price, other->bids[0].price);
        ++n;
    }
    EXPECT_FALSE(b.Next().has_value());
    EXPECT_EQ(n, 4u);
}

TEST(FeatherBboTape, Errors) {
    EXPECT_THROW(FeatherBboTape(kRoot, ""), std::invalid_argument);
    EXPECT_THROW(FeatherBboTape(kRoot, "NOPE"), std::runtime_error);
    EXPECT_THROW(FeatherBboTape(kRoot + "/missing", "MGCZ5"), std::runtime_error);
}

TEST(MakeTape, PicksAdapterByPath) {
    const auto feather = dataset::MakeTape(kRoot, "SILZ5");
    EXPECT_EQ(feather->FirstSymbol(), "SILZ5");
    EXPECT_NE(dynamic_cast<FeatherBboTape*>(feather.get()), nullptr);

    const auto jsonl =
        dataset::MakeTape(std::string(CMF_TESTDATA_DIR) + "/population/smoke.jsonl", "");
    EXPECT_EQ(dynamic_cast<FeatherBboTape*>(jsonl.get()), nullptr);
}

}  // namespace
