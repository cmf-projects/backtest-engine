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

}  // namespace population
