#pragma once

// Wire (Bus*) ↔ in-memory population types. P0: single component.

#include "population/order.hpp"
#include "population/population.hpp"
#include "population/slot.hpp"
#include "population/tick.hpp"
#include "transport/bus_messages.hpp"

#include <string>
#include <vector>

namespace transport {

[[nodiscard]] bool ParseDecimal(const std::string& text, double& out);
[[nodiscard]] std::string FormatDecimal(double value);

[[nodiscard]] population::SlotConfig ToSlotConfig(const BusSlotConfig& in);
[[nodiscard]] std::vector<population::SlotConfig> ToSlotConfigs(
    const std::vector<BusSlotConfig>& in);

[[nodiscard]] population::OrderBatch ToOrderBatch(const BusOrderBatch& in);
[[nodiscard]] BusTick ToBusTick(const population::TickView& in);
void ToBusTickInto(const population::TickView& in, BusTick& out);
void FormatDecimalInto(double value, std::string& out);

}  // namespace transport
