#pragma once

#include "population/types.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace population {

// In-memory history of file snapshots for one component (P0: full snaps).
// SnapshotFloor = oldest ts kept; head = newest. Used later for md_lag views.
// Does not drive Clock / Tape — FileBook appends here as it advances.
class TapeWindow {
public:
    // Append a newer snapshot (ts must be > head, or window empty).
    // Reuses pooled SnapshotEvent shells when available (level capacity kept).
    void Append(SnapshotEvent snap);

    // Drop snapshots with ts_ns < floor_ts_ns. Recycles into pool.
    void TrimBefore(std::uint64_t floor_ts_ns);

    [[nodiscard]] bool empty() const noexcept { return snaps_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return snaps_.size(); }

    // Oldest / newest event ts. Undefined if empty().
    [[nodiscard]] std::uint64_t floor_ts_ns() const;
    [[nodiscard]] std::uint64_t head_ts_ns() const;

    // Latest snapshot with ts_ns <= at_ts_ns.
    // nullptr if empty or at_ts_ns < floor. Pointer valid until Trim/Append.
    [[nodiscard]] const SnapshotEvent* AtOrBefore(std::uint64_t at_ts_ns) const;

private:
    std::deque<SnapshotEvent> snaps_;
    std::vector<SnapshotEvent> pool_;  // recycled by TrimBefore → Append
};

}  // namespace population
