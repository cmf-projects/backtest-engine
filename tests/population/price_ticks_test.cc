#include "population/price_ticks.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using population::PriceToTicks;
using population::PriceToWholeFrac;
using population::TicksToPrice;
using population::WholeFracToPrice;

namespace {

void ExpectOnGridRoundTrip(double price, double tick_size) {
    const long long ticks = PriceToTicks(price, tick_size);
    const auto wf = PriceToWholeFrac(price, tick_size);
    const long long tpu = PriceToTicks(1.0, tick_size);
    EXPECT_GE(wf.frac, 0);
    EXPECT_LT(wf.frac, tpu);
    EXPECT_EQ(wf.whole * tpu + wf.frac, ticks);
    EXPECT_DOUBLE_EQ(WholeFracToPrice(wf.whole, wf.frac, tick_size), TicksToPrice(ticks, tick_size));
}

}  // namespace

TEST(PriceTicks, KnownSplitsTickOneCent) {
    // 100.5 / 0.01 → 10050 ticks → whole 100 + frac 50
    const auto a = PriceToWholeFrac(100.5, 0.01);
    EXPECT_EQ(a.whole, 100);
    EXPECT_EQ(a.frac, 50);
    EXPECT_DOUBLE_EQ(WholeFracToPrice(a.whole, a.frac, 0.01), 100.5);

    const auto b = PriceToWholeFrac(100.0, 0.01);
    EXPECT_EQ(b.whole, 100);
    EXPECT_EQ(b.frac, 0);
    EXPECT_DOUBLE_EQ(WholeFracToPrice(b.whole, b.frac, 0.01), 100.0);

    const auto c = PriceToWholeFrac(0.01, 0.01);
    EXPECT_EQ(c.whole, 0);
    EXPECT_EQ(c.frac, 1);
    EXPECT_DOUBLE_EQ(WholeFracToPrice(c.whole, c.frac, 0.01), 0.01);
}

TEST(PriceTicks, KnownSplitsTickOne) {
    const auto a = PriceToWholeFrac(25000.0, 1.0);
    EXPECT_EQ(a.whole, 25000);
    EXPECT_EQ(a.frac, 0);
    EXPECT_DOUBLE_EQ(WholeFracToPrice(a.whole, a.frac, 1.0), 25000.0);

    const auto b = PriceToWholeFrac(1.0, 1.0);
    EXPECT_EQ(b.whole, 1);
    EXPECT_EQ(b.frac, 0);
}

TEST(PriceTicks, RoundTripManyOnGrid) {
    const std::vector<double> ticks{0.01, 0.05, 0.1, 1.0, 0.0001};
    for (double tick_size : ticks) {
        for (int i = -200; i <= 200; ++i) {
            const double price = static_cast<double>(i) * tick_size;
            ExpectOnGridRoundTrip(price, tick_size);
        }
        // Non-integer whole + mid-frac
        ExpectOnGridRoundTrip(123.0 + 7.0 * tick_size, tick_size);
    }
}

TEST(PriceTicks, OffGridSnapsViaLlround) {
    // Closer to 100.01 than 100.00 → one tick
    const auto wf = PriceToWholeFrac(100.005, 0.01);
    EXPECT_EQ(wf.whole, 100);
    EXPECT_EQ(wf.frac, 1);
    EXPECT_DOUBLE_EQ(WholeFracToPrice(wf.whole, wf.frac, 0.01), 100.01);
}

TEST(PriceTicks, NegativePricesNormalizedFrac) {
    // -100.5 / 0.01 → -10050 ticks → whole -101 + frac 50 (0 <= frac < tpu)
    const auto wf = PriceToWholeFrac(-100.5, 0.01);
    EXPECT_EQ(wf.whole, -101);
    EXPECT_EQ(wf.frac, 50);
    EXPECT_DOUBLE_EQ(WholeFracToPrice(wf.whole, wf.frac, 0.01), -100.5);
}

TEST(PriceTicks, RejectsBadTickSize) {
    EXPECT_THROW((void)PriceToTicks(1.0, 0.0), std::invalid_argument);
    EXPECT_THROW((void)PriceToWholeFrac(1.0, -0.01), std::invalid_argument);
    EXPECT_THROW((void)WholeFracToPrice(1, 0, NAN), std::invalid_argument);
}
