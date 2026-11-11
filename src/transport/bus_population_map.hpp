#pragma once

// Wire (Bus*) ↔ in-memory population types.

#include "population/order.hpp"
#include "population/population.hpp"
#include "population/slot.hpp"
#include "population/tick.hpp"
#include "transport/bus_messages.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace transport {

using TickSizeByComponent = std::unordered_map<std::string, double>;

[[nodiscard]] bool ParseDecimal(const std::string& text, double& out);

[[nodiscard]] population::SlotConfig ToSlotConfig(const BusSlotConfig& in);
[[nodiscard]] std::vector<population::SlotConfig> ToSlotConfigs(
    const std::vector<BusSlotConfig>& in);

[[nodiscard]] population::OrderBatch ToOrderBatch(const BusOrderBatch& in,
                                                  const TickSizeByComponent& tick_sizes);
void ToOrderBatchInto(const BusOrderBatch& in, population::OrderBatch& out,
                      const TickSizeByComponent& tick_sizes);
[[nodiscard]] BusTick ToBusTick(const population::TickView& in,
                                const TickSizeByComponent& tick_sizes);
void ToBusTickInto(const population::TickView& in, BusTick& out,
                   const TickSizeByComponent& tick_sizes);

[[nodiscard]] BusPrice ToBusPrice(double price, double tick_size);
[[nodiscard]] double FromBusPrice(const BusPrice& px, double tick_size);

}  // namespace transport
