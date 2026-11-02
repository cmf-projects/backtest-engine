#pragma once

#include "population/order.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <vector>

namespace population {

// Walk opposite levels while price crosses; mutates `opposite` and `working`.
// Emits Fill trades. Caller handles IOC leftover / FOK pre-check.
void MatchOne(WorkingOrder& working, std::vector<Level>& opposite, double tick_size,
              std::vector<Trade>& trades);

// True if opposite liquidity that crosses can cover qty (does not mutate).
[[nodiscard]] bool CanFullyFill(Side side, long long price_ticks, std::uint64_t qty,
                                const std::vector<Level>& opposite, double tick_size);

[[nodiscard]] long long PriceToTicks(double price, double tick_size);

}  // namespace population

