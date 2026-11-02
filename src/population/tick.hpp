#pragma once

#include "population/order.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace population {

// In-memory Tick (mirrors protos/messages.proto).
//
// Cadence:
// - `Tick.books` (shared) unused in P0 with per-slot md_lag.
// - Each slot: Keyframe on first send / Resync; else Diff of file view at
//   pop_ts - md_lag vs that slot's last sent book.

enum class BookPatchKind {
    Keyframe,
    Diff,
};

struct SharedBookView {
    std::string component_id;
    BookPatchKind kind = BookPatchKind::Keyframe;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

// Per-component book update inside one strategy's SlotTick.
struct SlotBookUpdateView {
    std::string component_id;
    BookPatchKind kind = BookPatchKind::Diff;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

struct SlotTickView {
    std::uint32_t slot_id = 0;
    std::vector<SlotBookUpdateView> books;
    std::vector<Trade> trades;  // fills/cancels from due match at this pop_ts
};

struct TickView {
    std::uint64_t pop_ts_ns = 0;
    std::vector<SharedBookView> books;  // shared seed / not used for per-slot resync
    std::vector<SlotTickView> slots;
};

}  // namespace population
