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
    // Widest md_lag across the population's slots. The ring is sized from this
    // and the smallest gap between snapshots seen on this component.
    void BindLag(std::uint64_t max_lag_ns) noexcept;

    // Append a newer snapshot (ts must be > head, or window empty).
    // Slots are a ring. Level vectors stay reserved across trim and append.
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
    // Snaps kept while trimmed: the one at or before t - lag, plus every snap
    // inside the lag. 1 + ceil(lag / min gap). More strategies do not add
    // slots; only the widest lag does. Grows further if the caller does not trim.
    [[nodiscard]] std::size_t SteadySlots() const noexcept;
    void EnsureRoom();
    void GrowTo(std::size_t next_n);

    [[nodiscard]] SnapshotEvent& At(std::size_t logical) noexcept;
    [[nodiscard]] const SnapshotEvent& At(std::size_t logical) const noexcept;
    static void ClearKept(SnapshotEvent& snap);

    std::vector<SnapshotEvent> slots_;
    std::size_t begin_ = 0;
    std::size_t size_ = 0;
    std::uint64_t max_lag_ns_ = 0;
    std::uint64_t min_gap_ns_ = 0;
};

}  // namespace population
