#include "population/tape_window.hpp"

#include <stdexcept>

namespace population {

void TapeWindow::Append(SnapshotEvent snap) {
    if (snap.ts_ns == 0) {
        throw std::invalid_argument("population::TapeWindow: ts_ns must be non-zero");
    }
    if (!snaps_.empty() && snap.ts_ns <= snaps_.back().ts_ns) {
        throw std::invalid_argument(
            "population::TapeWindow: Append requires ts > head (got " +
            std::to_string(snap.ts_ns) + " <= " + std::to_string(snaps_.back().ts_ns) + ")");
    }
    snaps_.push_back(std::move(snap));
}

void TapeWindow::TrimBefore(std::uint64_t floor_ts_ns) {
    while (!snaps_.empty() && snaps_.front().ts_ns < floor_ts_ns) {
        snaps_.pop_front();
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
    // Linear scan is fine for P0; snaps_ is ordered by ts.
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
