#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace population {

// SlotConfig from Open. Immutable for the run after Open.
struct SlotConfig {
    std::uint32_t slot_id = 0;
    std::uint64_t md_lag_ns = 0;
    std::uint64_t delta_ns = 0;      // MUST be >= 1
    double impact_lambda = 0.0;      // 0 → impact off
};

// Throws std::invalid_argument on Δ == 0 or duplicate slot_id.
void ValidateSlots(const std::vector<SlotConfig>& slots);

}  // namespace population
