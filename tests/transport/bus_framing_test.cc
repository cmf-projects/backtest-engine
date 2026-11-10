#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"

#include <gtest/gtest.h>

using transport::BusAssetRole;
using transport::BusBookPatchKind;
using transport::BusBookUpdate;
using transport::BusComponent;
using transport::BusExecType;
using transport::BusOpen;
using transport::BusSlotConfig;
using transport::BusSlotTick;
using transport::BusTick;
using transport::BusTrade;
using transport::DecodeBusOpen;
using transport::DecodeBusTick;
using transport::EncodeBusOpen;
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
    BusTick tick;
    tick.pop_ts_ns = 42;
    tick.books.push_back(BusBookUpdate{
        .component_id = "SPOT",
        .kind = BusBookPatchKind::Keyframe,
        .bids = {{.price = "100.5", .qty = 10}, {.price = "100.0", .qty = 5}},
        .asks = {{.price = "101.0", .qty = 7}},
    });
    BusSlotTick slot;
    slot.slot_id = 7;
    slot.books.push_back(BusBookUpdate{
        .component_id = "SPOT",
        .kind = BusBookPatchKind::Diff,
        .bids = {{.price = "100.5", .qty = 9}},
        .asks = {},
    });
    slot.trades.push_back(BusTrade{
        .client_order_id = 99,
        .exec_type = BusExecType::Fill,
        .component_id = "SPOT",
        .price = "100.5",
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
    EXPECT_EQ(got.books[0].bids[0].price, "100.5");
    ASSERT_EQ(got.slots.size(), 1u);
    EXPECT_EQ(got.slots[0].slot_id, 7u);
    ASSERT_EQ(got.slots[0].books.size(), 1u);
    EXPECT_EQ(got.slots[0].books[0].kind, BusBookPatchKind::Diff);
    ASSERT_EQ(got.slots[0].trades.size(), 1u);
    EXPECT_EQ(got.slots[0].trades[0].client_order_id, 99u);
    EXPECT_EQ(got.slots[0].trades[0].exec_type, BusExecType::Fill);
    EXPECT_EQ(got.slots[0].trades[0].qty, 1u);
}
