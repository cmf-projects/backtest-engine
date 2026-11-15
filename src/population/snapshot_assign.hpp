#pragma once

#include "population/types.hpp"

#include <utility>

namespace population {

// Copy/move snapshot fields while reusing destination level capacity.
inline void AssignSnapshot(SnapshotEvent& dst, const SnapshotEvent& src) {
    dst.seq = src.seq;
    dst.ts_ns = src.ts_ns;
    dst.symbol = src.symbol;
    dst.bids = src.bids;
    dst.asks = src.asks;
}

inline void AssignSnapshot(SnapshotEvent& dst, SnapshotEvent&& src) {
    dst.seq = src.seq;
    dst.ts_ns = src.ts_ns;
    dst.symbol = std::move(src.symbol);
    dst.bids.swap(src.bids);
    dst.asks.swap(src.asks);
    src.seq = 0;
    src.ts_ns = 0;
    src.symbol.clear();
    src.bids.clear();
    src.asks.clear();
}

}  // namespace population
