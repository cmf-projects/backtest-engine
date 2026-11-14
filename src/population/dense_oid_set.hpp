#pragma once

// Epoch-stamped dense set of (slot_id, client_order_id).
// Assumes client_order_id are dense uint64 (contract: per-pop from 1, increasing).
// Clear() is O(1) via stamp bump. Cell index is client_order_id - 1.
// Insert may grow the per-slot vector to client_order_id.

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace population {

inline constexpr std::size_t kOidSetCapacity = 16;

class DenseOidStampSet {
public:
    // One vector per slot, born with room for the first ids. Later Insert grows it.
    void ReserveSlot(std::uint32_t slot_id) { by_slot_[slot_id].reserve(kOidSetCapacity); }

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
        const std::uint64_t index = client_order_id - 1;
        if (it == by_slot_.end() || index >= it->second.size()) {
            return false;
        }
        return it->second[index] == stamp_;
    }

    // Returns false if (slot, oid) was already present in this epoch.
    bool Insert(std::uint32_t slot_id, std::uint64_t client_order_id) {
        if (client_order_id == 0) {
            return false;
        }
        auto& marks = by_slot_[slot_id];
        const std::uint64_t index = client_order_id - 1;
        if (marks.size() <= index) {
            marks.resize(client_order_id);
        }
        if (marks[index] == stamp_) {
            return false;
        }
        marks[index] = stamp_;
        return true;
    }

private:
    std::uint32_t stamp_ = 1;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> by_slot_;
};

}  // namespace population
