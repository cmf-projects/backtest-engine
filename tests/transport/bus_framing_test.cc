#include "population/types.hpp"
#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/wire_bytes.hpp"

#include <cstdint>
#include <initializer_list>
#include <vector>

#include <gtest/gtest.h>

using transport::BusAssetRole;
using transport::BusBookPatchKind;
using transport::BusBookLevel;
using transport::BusBookUpdate;
using transport::BusCancel;
using transport::BusComponent;
using transport::BusExecType;
using transport::BusOpen;
using transport::BusOrder;
using transport::BusOrderBatch;
using transport::BusPrice;
using transport::BusSide;
using transport::BusSlotConfig;
using transport::BusSlotTick;
using transport::BusTick;
using transport::BusTimeInForce;
using transport::BusTrade;
using transport::DecodeBusOpen;
using transport::DecodeBusOrderBatch;
using transport::DecodeBusTick;
using transport::EncodeBusOpen;
using transport::EncodeBusOrderBatch;
using transport::EncodeBusTick;
using transport::ValidateBusOpen;

TEST(BusFraming, OpenRoundTrip) {
    BusOpen open;
    open.population_id = "pop-wire";
    open.tape_stride = 2;
    open.components.push_back(BusComponent{
        .component_id = "SPOT",
        .role = BusAssetRole::Spot,
        .dataset_path = "/data/sber.jsonl",
        .class_code = "TQBR",
        .sec_code = "SBER",
        .tick_size = "0.01",
    });
    open.components.push_back(BusComponent{
        .component_id = "FUT",
        .role = BusAssetRole::Future,
        .dataset_path = "/data/fut.jsonl",
        .class_code = "FCEU",
        .sec_code = "Si-6.26",
        .tick_size = "1",
    });
    open.slots.push_back(BusSlotConfig{
        .slot_id = 1, .md_lag_ns = 100, .delta_ns = 4, .impact_lambda = "0.001"});
    open.slots.push_back(BusSlotConfig{
        .slot_id = 2, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = "0"});

    const auto bytes = EncodeBusOpen(open);
    BusOpen got;
    ASSERT_TRUE(DecodeBusOpen(bytes.data(), bytes.size(), got));
    EXPECT_EQ(got.population_id, open.population_id);
    EXPECT_EQ(got.tape_stride, 2u);
    ASSERT_EQ(got.components.size(), 2u);
    EXPECT_EQ(got.components[1].component_id, "FUT");
    EXPECT_EQ(got.components[1].role, BusAssetRole::Future);
    EXPECT_EQ(got.components[1].tick_size, "1");
    ASSERT_EQ(got.slots.size(), 2u);
    EXPECT_EQ(got.slots[0].delta_ns, 4u);
    EXPECT_EQ(got.slots[0].impact_lambda, "0.001");
    EXPECT_EQ(got.slots[1].slot_id, 2u);

    std::string why;
    EXPECT_TRUE(ValidateBusOpen(got, why));
}

TEST(BusFraming, OpenRejectsBadDelta) {
    BusOpen open;
    open.population_id = "x";
    open.components.push_back(BusComponent{
        .component_id = "SPOT",
        .role = BusAssetRole::Spot,
        .dataset_path = "a.jsonl",
        .tick_size = "0.01",
    });
    open.slots.push_back(
        BusSlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 0, .impact_lambda = "0"});
    std::string why;
    EXPECT_FALSE(ValidateBusOpen(open, why));
    EXPECT_EQ(why, "delta_ns == 0");
}

TEST(BusFraming, TickRoundTripWithBooksAndTrades) {
    // tick_size 0.01 → "100.5" = whole 100 + frac 50
    constexpr BusPrice k100_5{.whole = 100, .frac = 50};
    constexpr BusPrice k100{.whole = 100, .frac = 0};
    constexpr BusPrice k101{.whole = 101, .frac = 0};

    BusTick tick;
    tick.pop_ts_ns = 42;
    tick.n_books = 1;
    tick.books.push_back(BusBookUpdate{
        .component_id = "SPOT",
        .kind = BusBookPatchKind::Keyframe,
        .bids = {{.price = k100_5, .qty = 10}, {.price = k100, .qty = 5}},
        .asks = {{.price = k101, .qty = 7}},
    });
    BusSlotTick slot;
    slot.slot_id = 7;
    slot.n_books = 1;
    slot.books.push_back(BusBookUpdate{
        .component_id = "SPOT",
        .kind = BusBookPatchKind::Diff,
        .bids = {{.price = k100_5, .qty = 9}},
        .asks = {},
    });
    slot.trades.push_back(BusTrade{
        .client_order_id = 99,
        .exec_type = BusExecType::Fill,
        .component_id = "SPOT",
        .price = k100_5,
        .qty = 1,
        .leaves = 0,
    });
    tick.slots.push_back(std::move(slot));

    const auto bytes = EncodeBusTick(tick);
    BusTick got;
    ASSERT_TRUE(DecodeBusTick(bytes.data(), bytes.size(), got));
    EXPECT_EQ(got.pop_ts_ns, 42u);
    ASSERT_EQ(got.books.size(), 1u);
    EXPECT_EQ(got.books[0].kind, BusBookPatchKind::Keyframe);
    ASSERT_EQ(got.books[0].bids.size(), 2u);
    EXPECT_EQ(got.books[0].bids[0].price, k100_5);
    ASSERT_EQ(got.slots.size(), 1u);
    EXPECT_EQ(got.slots[0].slot_id, 7u);
    ASSERT_EQ(got.slots[0].books.size(), 1u);
    EXPECT_EQ(got.slots[0].books[0].kind, BusBookPatchKind::Diff);
    ASSERT_EQ(got.slots[0].trades.size(), 1u);
    EXPECT_EQ(got.slots[0].trades[0].client_order_id, 99u);
    EXPECT_EQ(got.slots[0].trades[0].exec_type, BusExecType::Fill);
    EXPECT_EQ(got.slots[0].trades[0].price, k100_5);
    EXPECT_EQ(got.slots[0].trades[0].qty, 1u);
}

TEST(BusFraming, KeyframeOfFortyBidsRoundTrips) {
    // A 40-level shift is sent as a keyframe, not an 80-row diff. The wire
    // accepts that book (a side longer than 64 would be rejected).
    BusTick tick;
    tick.pop_ts_ns = 200;
    BusSlotTick slot;
    slot.slot_id = 1;
    slot.n_books = 1;
    BusBookUpdate book;
    book.component_id = "C1";
    book.kind = BusBookPatchKind::Keyframe;
    book.bids.reserve(40);
    for (int i = 0; i < 40; ++i) {
        const std::int64_t ticks = 9960 - i;  // 99.60, 99.59, ...
        book.bids.push_back(BusBookLevel{
            .price = BusPrice{.whole = ticks / 100, .frac = ticks % 100},
            .qty = 10,
        });
    }
    book.asks.push_back(BusBookLevel{.price = BusPrice{.whole = 101, .frac = 0}, .qty = 5});
    slot.books.push_back(std::move(book));
    tick.slots.push_back(std::move(slot));

    const auto bytes = EncodeBusTick(tick);
    BusTick got;
    ASSERT_TRUE(DecodeBusTick(bytes.data(), bytes.size(), got));
    ASSERT_EQ(got.slots.size(), 1u);
    ASSERT_EQ(got.slots[0].books.size(), 1u);
    EXPECT_EQ(got.slots[0].books[0].kind, BusBookPatchKind::Keyframe);
    ASSERT_EQ(got.slots[0].books[0].bids.size(), 40u);
    EXPECT_EQ(got.slots[0].books[0].bids.front().price, (BusPrice{.whole = 99, .frac = 60}));
    EXPECT_EQ(got.slots[0].books[0].bids.back().price, (BusPrice{.whole = 99, .frac = 21}));
    ASSERT_EQ(got.slots[0].books[0].asks.size(), 1u);
    EXPECT_EQ(got.slots[0].books[0].asks[0].qty, 5u);
}

TEST(BusFraming, LivePrefixFollowsMessageOrder) {
    constexpr BusPrice k100{.whole = 100, .frac = 0};
    constexpr BusPrice k101{.whole = 101, .frac = 0};

    BusTick both;
    both.pop_ts_ns = 1;
    both.n_books = 2;
    both.books.push_back(BusBookUpdate{
        .component_id = "FUT",
        .kind = BusBookPatchKind::Keyframe,
        .bids = {{.price = k100, .qty = 1}},
        .asks = {{.price = k101, .qty = 1}},
    });
    both.books.push_back(BusBookUpdate{
        .component_id = "SPOT",
        .kind = BusBookPatchKind::Keyframe,
        .bids = {{.price = k100, .qty = 2}},
        .asks = {{.price = k101, .qty = 2}},
    });
    BusTick spot;
    spot.pop_ts_ns = 2;
    spot.n_books = 1;
    spot.books.push_back(BusBookUpdate{
        .component_id = "SPOT",
        .kind = BusBookPatchKind::Diff,
        .bids = {{.price = k100, .qty = 3}},
        .asks = {},
    });

    const auto both_bytes = EncodeBusTick(both);
    const auto spot_bytes = EncodeBusTick(spot);
    BusTick got;
    ASSERT_TRUE(DecodeBusTick(both_bytes.data(), both_bytes.size(), got));
    ASSERT_EQ(got.n_books, 2u);
    EXPECT_EQ(got.books[0].component_id, "FUT");

    ASSERT_TRUE(DecodeBusTick(spot_bytes.data(), spot_bytes.size(), got));
    EXPECT_EQ(got.n_books, 1u);
    EXPECT_EQ(got.books[0].component_id, "SPOT");
    EXPECT_EQ(got.books[0].kind, BusBookPatchKind::Diff);
    ASSERT_EQ(got.books[0].bids.size(), 1u);
    EXPECT_EQ(got.books[0].bids[0].qty, 3u);
    EXPECT_EQ(EncodeBusTick(got), spot_bytes);
}

TEST(BusFraming, BookStorageReusedForOneOrMoreComponents) {
    constexpr BusPrice k100{.whole = 100, .frac = 0};
    constexpr BusPrice k101{.whole = 101, .frac = 0};

    auto book = [](const char* id, BusBookPatchKind kind, std::uint64_t bid_qty) {
        return BusBookUpdate{
            .component_id = id,
            .kind = kind,
            .bids = {{.price = k100, .qty = bid_qty}},
            .asks = {{.price = k101, .qty = 1}},
        };
    };
    auto encode = [&](std::uint64_t ts, std::initializer_list<BusBookUpdate> books) {
        BusTick tick;
        tick.pop_ts_ns = ts;
        tick.n_books = static_cast<std::uint32_t>(books.size());
        tick.books.assign(books);
        return EncodeBusTick(tick);
    };

    const auto one = encode(1, {book("SPOT", BusBookPatchKind::Keyframe, 1)});
    const auto two = encode(2, {book("FUT", BusBookPatchKind::Keyframe, 2),
                                 book("SPOT", BusBookPatchKind::Keyframe, 3)});
    const auto spot_again = encode(3, {book("SPOT", BusBookPatchKind::Diff, 4)});

    BusTick got;
    ASSERT_TRUE(DecodeBusTick(one.data(), one.size(), got));
    ASSERT_TRUE(DecodeBusTick(two.data(), two.size(), got));
    ASSERT_EQ(got.n_books, 2u);

    const auto* books_data = got.books.data();
    const auto books_cap = got.books.capacity();
    const auto* fut_bids = got.books[0].bids.data();
    const auto* fut_asks = got.books[0].asks.data();
    const auto* spot_bids = got.books[1].bids.data();
    const auto* spot_asks = got.books[1].asks.data();
    const auto fut_bids_cap = got.books[0].bids.capacity();
    const auto spot_bids_cap = got.books[1].bids.capacity();

    ASSERT_TRUE(DecodeBusTick(spot_again.data(), spot_again.size(), got));
    EXPECT_EQ(got.n_books, 1u);
    EXPECT_EQ(got.books[0].component_id, "SPOT");
    EXPECT_EQ(got.books[0].kind, BusBookPatchKind::Diff);
    EXPECT_EQ(got.books.data(), books_data);
    EXPECT_EQ(got.books.capacity(), books_cap);
    EXPECT_EQ(got.books.size(), 2u);
    EXPECT_EQ(got.books[0].bids.data(), fut_bids);
    EXPECT_EQ(got.books[0].asks.data(), fut_asks);
    EXPECT_EQ(got.books[0].bids.capacity(), fut_bids_cap);
    EXPECT_EQ(got.books[1].bids.data(), spot_bids);
    EXPECT_EQ(got.books[1].asks.data(), spot_asks);
    EXPECT_EQ(got.books[1].bids.capacity(), spot_bids_cap);

    ASSERT_TRUE(DecodeBusTick(two.data(), two.size(), got));
    EXPECT_EQ(got.n_books, 2u);
    EXPECT_EQ(got.books[0].component_id, "FUT");
    EXPECT_EQ(got.books[1].component_id, "SPOT");
    EXPECT_EQ(got.books.data(), books_data);
    EXPECT_EQ(got.books[0].bids.data(), fut_bids);
    EXPECT_EQ(got.books[1].bids.data(), spot_bids);
}

TEST(BusFraming, MissingComponentClearsLevelsKeepsCapacity) {
    constexpr BusPrice k100{.whole = 100, .frac = 0};
    constexpr BusPrice k101{.whole = 101, .frac = 0};

    auto encode = [](bool with_fut) {
        BusTick tick;
        tick.pop_ts_ns = 1;
        tick.n_books = with_fut ? 2 : 1;
        tick.books.push_back(BusBookUpdate{
            .component_id = "SPOT",
            .kind = BusBookPatchKind::Keyframe,
            .bids = {{.price = k100, .qty = 3}},
            .asks = {{.price = k101, .qty = 4}},
        });
        if (with_fut) {
            tick.books.push_back(BusBookUpdate{
                .component_id = "FUT",
                .kind = BusBookPatchKind::Diff,
                .bids = {{.price = k100, .qty = 1}},
                .asks = {},
            });
        }
        return EncodeBusTick(tick);
    };

    const auto both = encode(true);
    BusTick got;
    ASSERT_TRUE(DecodeBusTick(both.data(), both.size(), got));
    ASSERT_EQ(got.books.size(), 2u);
    const std::size_t spot_cap = got.books[0].bids.capacity();
    EXPECT_GT(spot_cap, 0u);

    const auto spot_only = encode(false);
    ASSERT_TRUE(DecodeBusTick(spot_only.data(), spot_only.size(), got));
    EXPECT_EQ(got.n_books, 1u);
    ASSERT_EQ(got.books.size(), 2u);
    EXPECT_EQ(got.books[0].component_id, "SPOT");
    EXPECT_EQ(got.books[0].bids.size(), 1u);
    EXPECT_TRUE(got.books[1].bids.empty());
    EXPECT_TRUE(got.books[1].asks.empty());
    EXPECT_GE(got.books[1].bids.capacity(), 1u);
    EXPECT_EQ(got.books[0].bids.capacity(), spot_cap);
}

TEST(BusFraming, NoTradesClearsSizeKeepsCapacity) {
    constexpr BusPrice k100{.whole = 100, .frac = 0};

    auto encode = [](bool with_trade) {
        BusTick tick;
        tick.pop_ts_ns = 1;
        BusSlotTick slot;
        slot.slot_id = 1;
        if (with_trade) {
            slot.trades.push_back(BusTrade{
                .client_order_id = 7,
                .exec_type = BusExecType::Fill,
                .component_id = "SPOT",
                .price = k100,
                .qty = 1,
                .leaves = 0,
            });
        }
        tick.slots.push_back(std::move(slot));
        return EncodeBusTick(tick);
    };

    BusTick got;
    const auto with = encode(true);
    ASSERT_TRUE(DecodeBusTick(with.data(), with.size(), got));
    ASSERT_EQ(got.slots.size(), 1u);
    ASSERT_EQ(got.slots[0].trades.size(), 1u);
    const std::size_t cap = got.slots[0].trades.capacity();
    EXPECT_EQ(cap, population::kTradeCapacity);

    const auto none = encode(false);
    ASSERT_TRUE(DecodeBusTick(none.data(), none.size(), got));
    ASSERT_EQ(got.slots.size(), 1u);
    EXPECT_TRUE(got.slots[0].trades.empty());
    EXPECT_EQ(got.slots[0].trades.capacity(), cap);
}

TEST(BusFraming, FewerSlotsRejected) {
    auto encode = [](std::uint32_t n_slots) {
        BusTick tick;
        tick.pop_ts_ns = 1;
        for (std::uint32_t i = 0; i < n_slots; ++i) {
            BusSlotTick slot;
            slot.slot_id = i + 1;
            tick.slots.push_back(std::move(slot));
        }
        return EncodeBusTick(tick);
    };

    BusTick got;
    const auto two = encode(2);
    ASSERT_TRUE(DecodeBusTick(two.data(), two.size(), got));
    ASSERT_EQ(got.slots.size(), 2u);

    const auto one = encode(1);
    EXPECT_FALSE(DecodeBusTick(one.data(), one.size(), got));
    EXPECT_EQ(got.slots.size(), 2u);
}

TEST(BusFraming, OrderBatchRoundTripEmpty) {
    BusOrderBatch batch;
    const auto bytes = EncodeBusOrderBatch(batch);
    BusOrderBatch got;
    ASSERT_TRUE(DecodeBusOrderBatch(bytes.data(), bytes.size(), got));
    EXPECT_TRUE(got.orders.empty());
    EXPECT_TRUE(got.cancels.empty());
}

TEST(BusFraming, OrderBatchRoundTripOrdersAndCancels) {
    constexpr BusPrice k100_5{.whole = 100, .frac = 50};  // tick 0.01
    constexpr BusPrice k25000{.whole = 25000, .frac = 0};  // tick 1

    BusOrderBatch batch;
    batch.orders.push_back(BusOrder{
        .slot_id = 1,
        .client_order_id = 42,
        .component_id = "SPOT",
        .side = BusSide::Buy,
        .price = k100_5,
        .qty = 10,
        .tif = BusTimeInForce::Day,
    });
    batch.orders.push_back(BusOrder{
        .slot_id = 2,
        .client_order_id = 43,
        .component_id = "FUT",
        .side = BusSide::Sell,
        .price = k25000,
        .qty = 1,
        .tif = BusTimeInForce::Ioc,
    });
    batch.cancels.push_back(BusCancel{.slot_id = 1, .client_order_id = 7});
    batch.cancels.push_back(BusCancel{.slot_id = 2, .client_order_id = 8});

    const auto bytes = EncodeBusOrderBatch(batch);
    BusOrderBatch got;
    ASSERT_TRUE(DecodeBusOrderBatch(bytes.data(), bytes.size(), got));
    ASSERT_EQ(got.orders.size(), 2u);
    EXPECT_EQ(got.orders[0].slot_id, 1u);
    EXPECT_EQ(got.orders[0].client_order_id, 42u);
    EXPECT_EQ(got.orders[0].component_id, "SPOT");
    EXPECT_EQ(got.orders[0].side, BusSide::Buy);
    EXPECT_EQ(got.orders[0].price, k100_5);
    EXPECT_EQ(got.orders[0].qty, 10u);
    EXPECT_EQ(got.orders[0].tif, BusTimeInForce::Day);
    EXPECT_EQ(got.orders[1].side, BusSide::Sell);
    EXPECT_EQ(got.orders[1].price, k25000);
    EXPECT_EQ(got.orders[1].tif, BusTimeInForce::Ioc);
    ASSERT_EQ(got.cancels.size(), 2u);
    EXPECT_EQ(got.cancels[0].slot_id, 1u);
    EXPECT_EQ(got.cancels[0].client_order_id, 7u);
    EXPECT_EQ(got.cancels[1].client_order_id, 8u);
}

TEST(BusFraming, OrderBatchRejectsTruncated) {
    BusOrderBatch batch;
    batch.orders.push_back(BusOrder{
        .slot_id = 1,
        .client_order_id = 1,
        .component_id = "SPOT",
        .side = BusSide::Buy,
        .price = BusPrice{.whole = 1, .frac = 0},
        .qty = 1,
        .tif = BusTimeInForce::Day,
    });
    auto bytes = EncodeBusOrderBatch(batch);
    ASSERT_GT(bytes.size(), 4u);
    bytes.resize(bytes.size() - 1);
    BusOrderBatch got;
    EXPECT_FALSE(DecodeBusOrderBatch(bytes.data(), bytes.size(), got));
}

TEST(BusFraming, OrderBatchRejectsImpossibleOrderCount) {
    const std::uint8_t payload[] = {0xff, 0xff, 0xff, 0xff};
    BusOrderBatch got;
    EXPECT_FALSE(DecodeBusOrderBatch(payload, sizeof(payload), got));
    EXPECT_TRUE(got.orders.empty());
}

TEST(BusFraming, OrderBatchRejectsImpossibleCancelCount) {
    std::vector<std::uint8_t> payload;
    transport::AppendU32(payload, 0);
    transport::AppendU32(payload, 0xffffffffu);
    BusOrderBatch got;
    EXPECT_FALSE(DecodeBusOrderBatch(payload.data(), payload.size(), got));
    EXPECT_TRUE(got.cancels.empty());
}

TEST(BusFraming, OpenRejectsImpossibleComponentCount) {
    std::vector<std::uint8_t> payload;
    transport::AppendU32(payload, 0);  // empty population_id
    transport::AppendU32(payload, 0);  // tape_stride
    transport::AppendU32(payload, 0xffffffffu);
    BusOpen got;
    EXPECT_FALSE(DecodeBusOpen(payload.data(), payload.size(), got));
    EXPECT_TRUE(got.components.empty());
}

TEST(BusFraming, OpenRejectsMoreThanThreeComponents) {
    std::vector<std::uint8_t> payload;
    transport::AppendU32(payload, 0);
    transport::AppendU32(payload, 0);
    transport::AppendU32(payload, 4);
    payload.resize(payload.size() + 4 * 21, 0);  // four minimum components
    transport::AppendU32(payload, 0);            // n_slots
    BusOpen got;
    EXPECT_FALSE(DecodeBusOpen(payload.data(), payload.size(), got));
}

TEST(BusFraming, OpenRejectsImpossibleSlotCount) {
    std::vector<std::uint8_t> payload;
    transport::AppendU32(payload, 0);
    transport::AppendU32(payload, 0);
    transport::AppendU32(payload, 0);  // no components
    transport::AppendU32(payload, 0xffffffffu);
    BusOpen got;
    EXPECT_FALSE(DecodeBusOpen(payload.data(), payload.size(), got));
    EXPECT_TRUE(got.slots.empty());
}

TEST(BusFraming, TickRejectsImpossibleSlotCount) {
    std::vector<std::uint8_t> payload;
    transport::AppendU64(payload, 0);  // pop_ts
    transport::AppendU32(payload, 0);  // n_books
    transport::AppendU32(payload, 0xffffffffu);
    BusTick got;
    EXPECT_FALSE(DecodeBusTick(payload.data(), payload.size(), got));
    EXPECT_TRUE(got.slots.empty());
}

TEST(BusFraming, TickRejectsImpossibleTradeCount) {
    std::vector<std::uint8_t> payload;
    transport::AppendU64(payload, 0);
    transport::AppendU32(payload, 0);  // shared books
    transport::AppendU32(payload, 1);  // one slot
    transport::AppendU32(payload, 1);  // slot_id
    transport::AppendU32(payload, 0);  // slot books
    transport::AppendU32(payload, 0xffffffffu);
    BusTick got;
    EXPECT_FALSE(DecodeBusTick(payload.data(), payload.size(), got));
    ASSERT_EQ(got.slots.size(), 1u);
    EXPECT_TRUE(got.slots[0].trades.empty());
}
