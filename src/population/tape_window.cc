#include "population/tape_window.hpp"

#include <stdexcept>
#include <utility>

namespace population {
namespace {

}  // namespace

void TapeWindow::Append(SnapshotEvent snap) {
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
        SnapshotEvent& dst = snaps_.back();
        dst.seq = snap.seq;
        dst.ts_ns = snap.ts_ns;
        dst.symbol = std::move(snap.symbol);
        // Swap keeps both capacities in circulation (dst↔snap), then recycle snap.
        dst.bids.swap(snap.bids);
        dst.asks.swap(snap.asks);
        snap.seq = 0;
        snap.ts_ns = 0;
        snap.symbol.clear();
        snap.bids.clear();
        snap.asks.clear();
        pool_.push_back(std::move(snap));
        return;
    }
    if (snap.bids.capacity() < kMaxLevelsPerSide) {
        snap.bids.reserve(kMaxLevelsPerSide);
    }
    if (snap.asks.capacity() < kMaxLevelsPerSide) {
        snap.asks.reserve(kMaxLevelsPerSide);
    }
    snaps_.push_back(std::move(snap));
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
