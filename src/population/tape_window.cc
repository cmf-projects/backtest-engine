#include "population/tape_window.hpp"

#include "population/snapshot_assign.hpp"

#include <stdexcept>
#include <utility>

namespace population {

void TapeWindow::Append(const SnapshotEvent& snap) {
    if (snap.ts_ns == 0) {
        throw std::invalid_argument("population::TapeWindow: ts_ns must be non-zero");
    }
    if (!snaps_.empty() && snap.ts_ns <= snaps_.back().ts_ns) {
        throw std::invalid_argument(
            "population::TapeWindow: Append requires ts > head (got " +
            std::to_string(snap.ts_ns) + " <= " + std::to_string(snaps_.back().ts_ns) + ")");
    }
    if (!pool_.empty()) {
        snaps_.push_back(std::move(pool_.back()));
        pool_.pop_back();
        AssignSnapshot(snaps_.back(), snap);
        return;
    }
    SnapshotEvent copy = snap;
    if (copy.bids.capacity() < kMaxLevelsPerSide) {
        copy.bids.reserve(kMaxLevelsPerSide);
    }
    if (copy.asks.capacity() < kMaxLevelsPerSide) {
        copy.asks.reserve(kMaxLevelsPerSide);
    }
    snaps_.push_back(std::move(copy));
}

void TapeWindow::TrimBefore(std::uint64_t floor_ts_ns) {
    while (!snaps_.empty() && snaps_.front().ts_ns < floor_ts_ns) {
        pool_.push_back(std::move(snaps_.front()));
        snaps_.pop_front();
        SnapshotEvent& recycled = pool_.back();
        recycled.seq = 0;
        recycled.ts_ns = 0;
        recycled.symbol.clear();
        recycled.bids.clear();
        recycled.asks.clear();
        if (recycled.bids.capacity() < kMaxLevelsPerSide) {
            recycled.bids.reserve(kMaxLevelsPerSide);
        }
        if (recycled.asks.capacity() < kMaxLevelsPerSide) {
            recycled.asks.reserve(kMaxLevelsPerSide);
        }
    }
}

std::uint64_t TapeWindow::floor_ts_ns() const {
    if (snaps_.empty()) {
        throw std::logic_error("population::TapeWindow: empty");
    }
    return snaps_.front().ts_ns;
}

std::uint64_t TapeWindow::head_ts_ns() const {
    if (snaps_.empty()) {
        throw std::logic_error("population::TapeWindow: empty");
    }
    return snaps_.back().ts_ns;
}

const SnapshotEvent* TapeWindow::AtOrBefore(std::uint64_t at_ts_ns) const {
    if (snaps_.empty() || at_ts_ns < snaps_.front().ts_ns) {
        return nullptr;
    }
    const SnapshotEvent* best = nullptr;
    for (const SnapshotEvent& snap : snaps_) {
        if (snap.ts_ns > at_ts_ns) {
            break;
        }
        best = &snap;
    }
    return best;
}

}  // namespace population
