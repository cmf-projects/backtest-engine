#pragma once

#include "population/order.hpp"
#include "population/price_ticks.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <vector>

namespace population {

// Walk opposite levels while price crosses.
// `passive` (order already rested): fill all leaves at the order price and
// leave the file book alone — that book is what remains after the trade.
// Otherwise the order just arrived and lifts the file levels at their prices.
// Caller handles IOC leftover / FOK pre-check.
void MatchOne(WorkingOrder& working, std::vector<Level>& opposite, double tick_size,
              std::vector<Trade>& trades, bool passive);

// True if opposite liquidity that crosses can cover qty (does not mutate).
[[nodiscard]] bool CanFullyFill(Side side, long long price_ticks, std::uint64_t qty,
                                const std::vector<Level>& opposite, double tick_size);

}  // namespace population


