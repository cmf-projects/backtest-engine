#pragma once

// Epoch-stamped dense set of (slot_id, client_order_id).
// Assumes client_order_id are dense uint64 (contract: per-pop from 1, increasing).
// Clear() is O(1) via stamp bump; Insert may grow the per-slot vector to oid+1.

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace population {

class DenseOidStampSet {
public:
    void Clear() {
        if (++stamp_ == 0) {
            for (auto& [_, marks] : by_slot_) {
                std::fill(marks.begin(), marks.end(), 0);
            }
            stamp_ = 1;
        }
    }

    [[nodiscard]] bool Contains(std::uint32_t slot_id, std::uint64_t client_order_id) const {
        if (client_order_id == 0) {
            return false;
        }
        const auto it = by_slot_.find(slot_id);
        if (it == by_slot_.end() || client_order_id >= it->second.size()) {
            return false;
        }
        return it->second[client_order_id] == stamp_;
    }

    // Returns false if (slot, oid) was already present in this epoch.
    bool Insert(std::uint32_t slot_id, std::uint64_t client_order_id) {
        if (client_order_id == 0) {
            return false;
        }
        auto& marks = by_slot_[slot_id];
        if (client_order_id >= marks.size()) {
            marks.resize(client_order_id + 1);
        }
        if (marks[client_order_id] == stamp_) {
            return false;
        }
        marks[client_order_id] = stamp_;
        return true;
    }

private:
    std::uint32_t stamp_ = 1;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> by_slot_;
};

}  // namespace population
