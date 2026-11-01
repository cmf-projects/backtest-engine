#include "population/population.hpp"

#include "population/book_diff.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace population {

Population::Population(std::string component_id, double tick_size, TapePtr tape,
                       std::vector<SlotConfig> slots)
    : component_id_(std::move(component_id))
    , tick_size_(tick_size)
    , slots_(std::move(slots))
    , book_(std::move(tape)) {
    if (component_id_.empty()) {
        throw std::invalid_argument("population: component_id must be non-empty");
    }
    if (!(tick_size_ > 0.0)) {
        throw std::invalid_argument("population: tick_size must be > 0");
    }
    ValidateSlots(slots_);
}

bool Population::HasSlot(std::uint32_t slot_id) const {
    return std::any_of(slots_.begin(), slots_.end(),
                       [slot_id](const SlotConfig& s) { return s.slot_id == slot_id; });
}

void Population::Start() {
    if (started_) {
        throw std::logic_error("population: already started");
    }
    book_.Start(clock_);
    started_ = true;
}

void Population::QueueResync(std::uint32_t slot_id) {
    if (!HasSlot(slot_id)) {
        throw std::invalid_argument("population: QueueResync unknown slot_id " +
                                    std::to_string(slot_id));
    }
    resync_slots_.insert(slot_id);
}

TickView Population::MakeTick() {
    if (!started_) {
        throw std::logic_error("population: MakeTick before Start");
    }

    TickView tick;
    tick.pop_ts_ns = clock_.pop_ts_ns();
    tick.slots.reserve(slots_.size());

    if (!sent_initial_keyframe_) {
        // Shared seed for all strategies once.
        SharedBookView shared;
        shared.component_id = component_id_;
        shared.kind = BookPatchKind::Keyframe;
        shared.bids = book_.bids();
        shared.asks = book_.asks();
        tick.books.push_back(std::move(shared));

        for (const SlotConfig& cfg : slots_) {
            SlotTickView slot_tick;
            slot_tick.slot_id = cfg.slot_id;
            tick.slots.push_back(std::move(slot_tick));
        }

        last_sent_bids_ = book_.bids();
        last_sent_asks_ = book_.asks();
        sent_initial_keyframe_ = true;
        resync_slots_.clear();
        return tick;
    }

    // After the seed: tick.books stays empty; patches are per-slot only.
    // PreferKeyframe → full file Keyframe; risk if PS merges resting into local
    // book instead of overlay — see docs/BUG_KEYFRAME_OVERLAY.md.
    const BookDiffResult diff =
        DiffBooks(last_sent_bids_, last_sent_asks_, book_.bids(), book_.asks(), tick_size_);

    for (const SlotConfig& cfg : slots_) {
        SlotTickView slot_tick;
        slot_tick.slot_id = cfg.slot_id;

        const bool slot_resync = resync_slots_.erase(cfg.slot_id) > 0;

        if (slot_resync) {
            SlotBookUpdateView patch;
            patch.component_id = component_id_;
            patch.kind = BookPatchKind::Keyframe;
            patch.bids = book_.bids();
            patch.asks = book_.asks();
            slot_tick.books.push_back(std::move(patch));
        } else if (!diff.empty()) {
            SlotBookUpdateView patch;
            patch.component_id = component_id_;
            patch.kind = BookPatchKind::Diff;
            patch.bids = diff.bids;
            patch.asks = diff.asks;
            slot_tick.books.push_back(std::move(patch));
        }

        tick.slots.push_back(std::move(slot_tick));
    }

    if (!diff.empty()) {
        last_sent_bids_ = book_.bids();
        last_sent_asks_ = book_.asks();
    }
    return tick;
}

bool Population::Advance() {
    if (!started_) {
        throw std::logic_error("population: Advance before Start");
    }
    return book_.Advance(clock_);
}

}  // namespace population
