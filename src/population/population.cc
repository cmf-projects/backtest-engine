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
    for (const SlotConfig& cfg : slots_) {
        slot_send_.emplace(cfg.slot_id, SlotSendState{});
    }
}

bool Population::HasSlot(std::uint32_t slot_id) const {
    return slot_send_.find(slot_id) != slot_send_.end();
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

SnapshotEvent Population::ViewSnap(const SlotConfig& cfg) const {
    const std::uint64_t pop = clock_.pop_ts_ns();
    const std::uint64_t view_ts =
        (cfg.md_lag_ns >= pop) ? 0 : (pop - cfg.md_lag_ns);

    if (auto snap = book_.window().AtOrBefore(view_ts)) {
        return *snap;
    }
    // Warm-up: lag reaches before SnapshotFloor → clamp to oldest kept snap.
    if (book_.window().empty()) {
        throw std::logic_error("population: ViewSnap on empty TapeWindow");
    }
    auto floor = book_.window().AtOrBefore(book_.window().floor_ts_ns());
    if (!floor) {
        throw std::logic_error("population: TapeWindow floor missing");
    }
    return *floor;
}

void Population::TrimWindowForLags() {
    if (book_.window().empty() || slots_.empty()) {
        return;
    }
    std::uint64_t max_lag = 0;
    for (const SlotConfig& cfg : slots_) {
        max_lag = std::max(max_lag, cfg.md_lag_ns);
    }
    const std::uint64_t pop = clock_.pop_ts_ns();
    // Warm-up (max_lag >= pop → view_ts under floor): nothing to drop yet.
    if (max_lag >= pop) {
        return;
    }
    if (auto cover = book_.window().AtOrBefore(pop - max_lag)) {
        book_.window().TrimBefore(cover->ts_ns);
    }
}

TickView Population::MakeTick() {
    if (!started_) {
        throw std::logic_error("population: MakeTick before Start");
    }

    TickView tick;
    tick.pop_ts_ns = clock_.pop_ts_ns();
    tick.slots.reserve(slots_.size());
    // Shared tick.books unused when md_lag may differ per slot.

    for (const SlotConfig& cfg : slots_) {
        SlotTickView slot_tick;
        slot_tick.slot_id = cfg.slot_id;

        const SnapshotEvent view = ViewSnap(cfg);
        SlotSendState& sent = slot_send_.at(cfg.slot_id);
        const bool slot_resync = resync_slots_.erase(cfg.slot_id) > 0;
        const bool need_keyframe = !sent.has_sent || slot_resync;

        if (need_keyframe) {
            SlotBookUpdateView patch;
            patch.component_id = component_id_;
            patch.kind = BookPatchKind::Keyframe;
            patch.bids = view.bids;
            patch.asks = view.asks;
            slot_tick.books.push_back(std::move(patch));
            sent.last_bids = view.bids;
            sent.last_asks = view.asks;
            sent.has_sent = true;
        } else {
            const BookDiffResult diff =
                DiffBooks(sent.last_bids, sent.last_asks, view.bids, view.asks, tick_size_);
            if (!diff.empty()) {
                SlotBookUpdateView patch;
                patch.component_id = component_id_;
                patch.kind = BookPatchKind::Diff;
                patch.bids = diff.bids;
                patch.asks = diff.asks;
                slot_tick.books.push_back(std::move(patch));
                sent.last_bids = view.bids;
                sent.last_asks = view.asks;
            }
        }

        tick.slots.push_back(std::move(slot_tick));
    }

    TrimWindowForLags();
    return tick;
}

bool Population::Advance() {
    if (!started_) {
        throw std::logic_error("population: Advance before Start");
    }
    return book_.Advance(clock_);
}

}  // namespace population
