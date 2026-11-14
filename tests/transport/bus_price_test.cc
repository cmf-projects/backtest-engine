#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/bus_population_map.hpp"

#include "population/price_ticks.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

using population::PriceToTicks;
using transport::BusBookPatchKind;
using transport::BusBookUpdate;
using transport::BusExecType;
using transport::BusOrder;
using transport::BusOrderBatch;
using transport::BusPrice;
using transport::BusSide;
using transport::BusSlotTick;
using transport::BusTick;
using transport::BusTimeInForce;
using transport::BusTrade;
using transport::DecodeBusOrderBatch;
using transport::DecodeBusTick;
using transport::EncodeBusOrderBatch;
using transport::EncodeBusTick;
using transport::FromBusPrice;
using transport::TickSizeByComponent;
using transport::ToBusPrice;
using transport::ToOrderBatch;

namespace {

[[nodiscard]] bool Near(double a, double b, double eps = 1e-12) {
    return std::abs(a - b) <= eps;
}

}  // namespace

TEST(BusPriceMap, KnownSplitsMatchHelpers) {
    const BusPrice a = ToBusPrice(100.5, 0.01);
    EXPECT_EQ(a.whole, 100);
    EXPECT_EQ(a.frac, 50);
    EXPECT_DOUBLE_EQ(FromBusPrice(a, 0.01), 100.5);

    const BusPrice b = ToBusPrice(25000.0, 1.0);
    EXPECT_EQ(b.whole, 25000);
    EXPECT_EQ(b.frac, 0);
    EXPECT_DOUBLE_EQ(FromBusPrice(b, 1.0), 25000.0);
}

TEST(BusPriceMap, RoundTripPreservesOnGridPrice) {
    const std::vector<double> tick_sizes{0.01, 0.05, 1.0, 0.0001};
    for (double tick_size : tick_sizes) {
        for (int i = 0; i <= 500; ++i) {
            const double price = static_cast<double>(i) * tick_size;
            const BusPrice px = ToBusPrice(price, tick_size);
            const double back = FromBusPrice(px, tick_size);
            EXPECT_TRUE(Near(back, price)) << "tick=" << tick_size << " price=" << price
                                           << " back=" << back;
            EXPECT_EQ(PriceToTicks(back, tick_size), PriceToTicks(price, tick_size));
        }
    }
}

TEST(BusPriceMap, NoExtraPrecisionBeyondTickGrid) {
    // Off-grid input snaps; restored value is on-grid, not the noisy double.
    const double noisy = 100.0 + 1e-12;
    const BusPrice px = ToBusPrice(noisy, 0.01);
    EXPECT_EQ(px.whole, 100);
    EXPECT_EQ(px.frac, 0);
    EXPECT_DOUBLE_EQ(FromBusPrice(px, 0.01), 100.0);

    const double mid = 100.005;  // rounds to 100.01
    const BusPrice mid_px = ToBusPrice(mid, 0.01);
    EXPECT_EQ(mid_px, (BusPrice{.whole = 100, .frac = 1}));
    EXPECT_DOUBLE_EQ(FromBusPrice(mid_px, 0.01), 100.01);
}

TEST(BusPriceMap, OrderBatchUsesPerComponentTickSize) {
    TickSizeByComponent ticks{{"SPOT", 0.01}, {"FUT", 1.0}};

    BusOrderBatch batch;
    batch.orders.push_back(BusOrder{
        .slot_id = 1,
        .client_order_id = 1,
        .component_id = "SPOT",
        .side = BusSide::Buy,
        .price = BusPrice{.whole = 100, .frac = 50},
        .qty = 1,
        .tif = BusTimeInForce::Day,
    });
    batch.orders.push_back(BusOrder{
        .slot_id = 1,
        .client_order_id = 2,
        .component_id = "FUT",
        .side = BusSide::Sell,
        .price = BusPrice{.whole = 25000, .frac = 0},
        .qty = 1,
        .tif = BusTimeInForce::Ioc,
    });

    const auto domain = ToOrderBatch(batch, ticks);
    ASSERT_EQ(domain.orders.size(), 2u);
    EXPECT_DOUBLE_EQ(domain.orders[0].price, 100.5);
    EXPECT_DOUBLE_EQ(domain.orders[1].price, 25000.0);
}

TEST(BusPriceWire, TickAndOrderBatchRoundTrip) {
    constexpr BusPrice k100_5{.whole = 100, .frac = 50};
    constexpr BusPrice k101{.whole = 101, .frac = 0};
    constexpr BusPrice k25000{.whole = 25000, .frac = 0};

    BusTick tick;
    tick.pop_ts_ns = 7;
    tick.n_books = 1;
    tick.books.push_back(BusBookUpdate{
        .component_id = "SPOT",
        .kind = BusBookPatchKind::Keyframe,
        .bids = {{.price = k100_5, .qty = 3}},
        .asks = {{.price = k101, .qty = 4}},
    });
    BusSlotTick slot;
    slot.slot_id = 1;
    slot.trades.push_back(BusTrade{
        .client_order_id = 9,
        .exec_type = BusExecType::Fill,
        .component_id = "SPOT",
        .price = k100_5,
        .qty = 1,
        .leaves = 0,
    });
    tick.slots.push_back(std::move(slot));

    const auto tick_bytes = EncodeBusTick(tick);
    BusTick tick_got;
    ASSERT_TRUE(DecodeBusTick(tick_bytes.data(), tick_bytes.size(), tick_got));
    ASSERT_EQ(tick_got.books.size(), 1u);
    EXPECT_EQ(tick_got.books[0].bids[0].price, k100_5);
    EXPECT_EQ(tick_got.books[0].asks[0].price, k101);
    ASSERT_EQ(tick_got.slots[0].trades.size(), 1u);
    EXPECT_EQ(tick_got.slots[0].trades[0].price, k100_5);

    BusOrderBatch batch;
    batch.orders.push_back(BusOrder{
        .slot_id = 1,
        .client_order_id = 1,
        .component_id = "FUT",
        .side = BusSide::Buy,
        .price = k25000,
        .qty = 2,
        .tif = BusTimeInForce::Day,
    });
    const auto batch_bytes = EncodeBusOrderBatch(batch);
    BusOrderBatch batch_got;
    ASSERT_TRUE(DecodeBusOrderBatch(batch_bytes.data(), batch_bytes.size(), batch_got));
    ASSERT_EQ(batch_got.orders.size(), 1u);
    EXPECT_EQ(batch_got.orders[0].price, k25000);

    // Restored domain price after wire still matches grid.
    EXPECT_DOUBLE_EQ(FromBusPrice(batch_got.orders[0].price, 1.0), 25000.0);
    EXPECT_DOUBLE_EQ(FromBusPrice(tick_got.books[0].bids[0].price, 0.01), 100.5);
}

TEST(BusPriceWire, EncodeUsesFixedWidthIntsNotDecimalText) {
    BusOrderBatch batch;
    batch.orders.push_back(BusOrder{
        .slot_id = 1,
        .client_order_id = 1,
        .component_id = "SPOT",
        .side = BusSide::Buy,
        .price = BusPrice{.whole = 100, .frac = 50},
        .qty = 1,
        .tif = BusTimeInForce::Day,
    });
    const auto bytes = EncodeBusOrderBatch(batch);
    const std::string blob(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // Price is two i64s — no decimal text like the old string wire.
    EXPECT_EQ(blob.find("100.5"), std::string::npos);
    EXPECT_EQ(blob.find('.'), std::string::npos);
}
