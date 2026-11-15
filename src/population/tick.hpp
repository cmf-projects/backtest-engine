#pragma once

#include "population/order.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace population {

// In-memory Tick (population ↔ transport map).
//
// Cadence:
// - `Tick.books`: one shared keyframe per component on the first tick,
//   when every slot still sees that same snap.
// - Later ticks: per-slot Diff of the slot's own view against the book it
//   was seeded with. A side longer than 64 rows is a per-slot Keyframe.

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
    std::vector<SharedBookView> books;  // shared seed (unused when md_lag differs)
    std::vector<SlotTickView> slots;
};

}  // namespace population
