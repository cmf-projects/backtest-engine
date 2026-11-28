#include "population/tape_window.hpp"

#include "population/snapshot_assign.hpp"

#include <stdexcept>
#include <utility>

namespace population {

SnapshotEvent& TapeWindow::At(std::size_t logical) noexcept {
    return slots_[(begin_ + logical) % slots_.size()];
}

const SnapshotEvent& TapeWindow::At(std::size_t logical) const noexcept {
    return slots_[(begin_ + logical) % slots_.size()];
}

void TapeWindow::ClearKept(SnapshotEvent& snap) {
    snap.seq = 0;
    snap.ts_ns = 0;
    snap.symbol.clear();
    snap.bids.clear();
    snap.asks.clear();
}

void TapeWindow::BindLag(std::uint64_t max_lag_ns) noexcept { max_lag_ns_ = max_lag_ns; }

void TapeWindow::EnsureRoom() {
    if (size_ < slots_.size()) {
        return;
    }
    if (begin_ == 0) {
        slots_.emplace_back();
        return;
    }
    std::vector<SnapshotEvent> next;
    next.reserve(size_ + 1);
    for (std::size_t i = 0; i < size_; ++i) {
        next.push_back(std::move(At(i)));
    }
    next.emplace_back();
    slots_ = std::move(next);
    begin_ = 0;
}

void TapeWindow::Append(const SnapshotEvent& snap) {
    if (snap.ts_ns == 0) {
        throw std::invalid_argument("population::TapeWindow: ts_ns must be non-zero");
    }
    if (size_ != 0 && snap.ts_ns <= At(size_ - 1).ts_ns) {
        throw std::invalid_argument(
            "population::TapeWindow: Append requires ts > head (got " +
            std::to_string(snap.ts_ns) + " <= " + std::to_string(At(size_ - 1).ts_ns) + ")");
    }
    EnsureRoom();
    SnapshotEvent& dest = At(size_);
    AssignSnapshot(dest, snap);
    ++size_;
}

void TapeWindow::TrimBefore(std::uint64_t floor_ts_ns) {
    while (size_ != 0 && At(0).ts_ns < floor_ts_ns) {
        ClearKept(At(0));
        begin_ = (begin_ + 1) % slots_.size();
        --size_;
    }
}

std::uint64_t TapeWindow::floor_ts_ns() const {
    if (size_ == 0) {
        throw std::logic_error("population::TapeWindow: empty");
    }
    return At(0).ts_ns;
}

std::uint64_t TapeWindow::head_ts_ns() const {
    if (size_ == 0) {
        throw std::logic_error("population::TapeWindow: empty");
    }
    return At(size_ - 1).ts_ns;
}

const SnapshotEvent* TapeWindow::AtOrBefore(std::uint64_t at_ts_ns) const {
    if (size_ == 0 || at_ts_ns < At(0).ts_ns) {
        return nullptr;
    }
    const SnapshotEvent* best = nullptr;
    for (std::size_t i = 0; i < size_; ++i) {
        if (At(i).ts_ns > at_ts_ns) {
            break;
        }
        best = &At(i);
    }
    return best;
}

}  // namespace population
