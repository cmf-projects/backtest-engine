#include "population/slot.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace population {

void ValidateSlots(const std::vector<SlotConfig>& slots) {
    if (slots.empty()) {
        throw std::invalid_argument("population: Open requires at least one slot");
    }
    std::unordered_set<std::uint32_t> seen;
    for (const SlotConfig& slot : slots) {
        if (slot.delta_ns == 0) {
            throw std::invalid_argument("population: slot " + std::to_string(slot.slot_id) +
                                       " has delta_ns == 0 (illegal)");
        }
        if (!seen.insert(slot.slot_id).second) {
            throw std::invalid_argument("population: duplicate slot_id " +
                                       std::to_string(slot.slot_id));
        }
    }
}

}  // namespace population
