#pragma once

#include "population/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace population {

// In-memory Tick (mirrors protos/messages.proto).
//
// Cadence:
// - Tick 1: `books` = shared Keyframe for everyone; slot books empty.
// - Later: `books` empty; each SlotTickView gets Diff patches (or Keyframe
//   if that slot needs a rebuild — large change or ResyncRequest).

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
    // trades: chunk 5
};

struct TickView {
    std::uint64_t pop_ts_ns = 0;
    std::vector<SharedBookView> books;  // shared seed / not used for per-slot resync
    std::vector<SlotTickView> slots;
};

}  // namespace population
