#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace population {

inline constexpr std::size_t kMaxLevelsPerSide = 64;

struct Level {
    double price = 0.0;
    std::uint64_t qty = 0;
};

// One snapshot record from a component tape (file book, no impact/resting).
struct SnapshotEvent {
    std::uint64_t seq = 0;
    std::uint64_t ts_ns = 0;
    std::string symbol;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

}  // namespace population
