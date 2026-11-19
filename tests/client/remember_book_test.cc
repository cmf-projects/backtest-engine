#include "orders.hpp"

#include <gtest/gtest.h>

namespace {

transport::BusPrice Px(std::int64_t whole) { return transport::BusPrice{.whole = whole, .frac = 0}; }

transport::BusTick TickWith(transport::BusBookUpdate update) {
    transport::BusTick tick;
    tick.pop_ts_ns = 1;
    tick.n_books = 1;
    tick.books.push_back(std::move(update));
    return tick;
}

void Remember(const transport::BusTick& tick, std::vector<client::RememberedBook>& memory) {
    std::uint64_t next_oid = 1;
    client::DensePending pending;
    transport::BusOrderBatch batch;
    client::MakeTakeBatch(tick, /*tick_n=*/1, /*order_every=*/4, next_oid, pending, batch, memory);
    EXPECT_TRUE(batch.orders.empty());
}

}  // namespace

TEST(RememberBook, KeyframeKeepsTheFirstLevel) {
    transport::BusBookUpdate update;
    update.component_id = "SPOT";
    update.kind = transport::BusBookPatchKind::Keyframe;
    update.bids = {{Px(101), 5}, {Px(100), 9}};
    update.asks = {{Px(200), 4}, {Px(201), 8}};

    std::vector<client::RememberedBook> memory;
    Remember(TickWith(std::move(update)), memory);

    ASSERT_EQ(memory.size(), 1u);
    EXPECT_TRUE(memory[0].has_bid);
    EXPECT_EQ(memory[0].bid, Px(101));
    EXPECT_EQ(memory[0].bid_qty, 5u);
    EXPECT_TRUE(memory[0].has_ask);
    EXPECT_EQ(memory[0].ask, Px(200));
    EXPECT_EQ(memory[0].ask_qty, 4u);
}

TEST(RememberBook, DiffKeepsTheBetterTouch) {
    transport::BusBookUpdate key;
    key.component_id = "SPOT";
    key.kind = transport::BusBookPatchKind::Keyframe;
    key.bids = {{Px(100), 2}};
    key.asks = {{Px(110), 2}};

    std::vector<client::RememberedBook> memory;
    Remember(TickWith(std::move(key)), memory);

    transport::BusBookUpdate diff;
    diff.component_id = "SPOT";
    diff.kind = transport::BusBookPatchKind::Diff;
    diff.bids = {{Px(100), 4}, {Px(99), 8}};
    diff.asks = {{Px(111), 1}, {Px(109), 3}};
    Remember(TickWith(std::move(diff)), memory);

    ASSERT_EQ(memory.size(), 1u);
    EXPECT_EQ(memory[0].bid, Px(100));
    EXPECT_EQ(memory[0].bid_qty, 4u);
    EXPECT_EQ(memory[0].ask, Px(109));
    EXPECT_EQ(memory[0].ask_qty, 3u);
}

TEST(RememberBook, PullOfTheTouchClearsTheSide) {
    transport::BusBookUpdate key;
    key.component_id = "SPOT";
    key.kind = transport::BusBookPatchKind::Keyframe;
    key.bids = {{Px(100), 2}};
    key.asks = {{Px(110), 2}};

    std::vector<client::RememberedBook> memory;
    Remember(TickWith(std::move(key)), memory);

    transport::BusBookUpdate diff;
    diff.component_id = "SPOT";
    diff.kind = transport::BusBookPatchKind::Diff;
    diff.bids = {{Px(100), 0}};
    diff.asks = {{Px(111), 0}};
    Remember(TickWith(std::move(diff)), memory);

    ASSERT_EQ(memory.size(), 1u);
    EXPECT_FALSE(memory[0].has_bid);
    EXPECT_EQ(memory[0].bid_qty, 0u);
    EXPECT_TRUE(memory[0].has_ask);
    EXPECT_EQ(memory[0].ask, Px(110));
}
