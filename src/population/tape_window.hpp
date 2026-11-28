#pragma once

#include "population/types.hpp"

#include <cstdint>
#include <vector>

namespace population {

// In-memory history of file snapshots for one component (P0: full snaps).
// SnapshotFloor = oldest ts kept; head = newest. Used later for md_lag views.
// Does not drive Clock / Tape — FileBook appends here as it advances.
class TapeWindow {
public:
    // Widest md_lag across the population's slots.
    void BindLag(std::uint64_t max_lag_ns) noexcept;

    // Append a newer snapshot (ts must be > head, or window empty).
    // A new ring slot is constructed only when this snap does not fit.
    void Append(const SnapshotEvent& snap);

    // Drop snapshots with ts_ns < floor_ts_ns. The slot stays in the ring.
    void TrimBefore(std::uint64_t floor_ts_ns);

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Oldest / newest event ts. Undefined if empty().
    [[nodiscard]] std::uint64_t floor_ts_ns() const;
    [[nodiscard]] std::uint64_t head_ts_ns() const;

    // Latest snapshot with ts_ns <= at_ts_ns.
    // nullptr if empty or at_ts_ns < floor. Pointer valid until Trim/Append.
    [[nodiscard]] const SnapshotEvent* AtOrBefore(std::uint64_t at_ts_ns) const;

private:
    // Reuse a trimmed slot. If the ring is full, construct exactly one slot
    // for the snapshot about to be written.
    void EnsureRoom();

    [[nodiscard]] SnapshotEvent& At(std::size_t logical) noexcept;
    [[nodiscard]] const SnapshotEvent& At(std::size_t logical) const noexcept;
    static void ClearKept(SnapshotEvent& snap);

    std::vector<SnapshotEvent> slots_;
    std::size_t begin_ = 0;
    std::size_t size_ = 0;
    std::uint64_t max_lag_ns_ = 0;
};

}  // namespace population
