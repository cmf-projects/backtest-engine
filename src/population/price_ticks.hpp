#pragma once

// Single price ↔ tick grid helper for the population path (IMPL 4.1).

#include <cmath>
#include <stdexcept>

namespace population {

[[nodiscard]] inline long long PriceToTicks(double price, double tick_size) {
    if (!(tick_size > 0.0) || !std::isfinite(tick_size) || !std::isfinite(price)) {
        throw std::invalid_argument("population::PriceToTicks: invalid price/tick_size");
    }
    return static_cast<long long>(std::llround(price / tick_size));
}

[[nodiscard]] inline double TicksToPrice(long long ticks, double tick_size) {
    if (!(tick_size > 0.0) || !std::isfinite(tick_size)) {
        throw std::invalid_argument("population::TicksToPrice: invalid tick_size");
    }
    return static_cast<double>(ticks) * tick_size;
}

// Wire split: price = whole + frac * tick_size, with 0 <= frac < ticks_per_unit.
// ticks_per_unit = PriceToTicks(1.0, tick_size). No extra precision beyond the grid.
struct WholeFrac {
    long long whole = 0;
    long long frac = 0;
};

[[nodiscard]] inline WholeFrac PriceToWholeFrac(double price, double tick_size) {
    const long long ticks = PriceToTicks(price, tick_size);
    const long long tpu = PriceToTicks(1.0, tick_size);
    if (tpu <= 0) {
        throw std::invalid_argument("population::PriceToWholeFrac: bad ticks_per_unit");
    }
    WholeFrac out;
    out.whole = ticks / tpu;
    out.frac = ticks % tpu;
    if (out.frac < 0) {  // towards-zero div on negatives
        out.frac += tpu;
        --out.whole;
    }
    return out;
}

[[nodiscard]] inline double WholeFracToPrice(long long whole, long long frac, double tick_size) {
    const long long tpu = PriceToTicks(1.0, tick_size);
    if (tpu <= 0) {
        throw std::invalid_argument("population::WholeFracToPrice: bad ticks_per_unit");
    }
    return TicksToPrice(whole * tpu + frac, tick_size);
}

}  // namespace population
