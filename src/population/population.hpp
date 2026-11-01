#pragma once

#include "population/clock.hpp"
#include "population/file_book.hpp"
#include "population/slot.hpp"
#include "population/tape.hpp"
#include "population/tick.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace population {

// Single-component population run (P0): clock + file book + slots → TickView.
class Population {
public:
    Population(std::string component_id, double tick_size, TapePtr tape,
               std::vector<SlotConfig> slots);

    void Start();

    // Strategy dropped its local book → next MakeTick sends Keyframe to this slot only.
    void QueueResync(std::uint32_t slot_id);

    // 1st tick: shared Keyframe in tick.books.
    // Later: per-slot Diff, or per-slot Keyframe (QueueResync).
    [[nodiscard]] TickView MakeTick();

    [[nodiscard]] bool Advance();

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool ended() const noexcept { return book_.ended(); }
    [[nodiscard]] const Clock& clock() const noexcept { return clock_; }
    [[nodiscard]] const std::vector<SlotConfig>& slots() const noexcept { return slots_; }
    [[nodiscard]] const std::string& component_id() const noexcept { return component_id_; }

private:
    [[nodiscard]] bool HasSlot(std::uint32_t slot_id) const;

    std::string component_id_;
    double tick_size_ = 0.0;
    std::vector<SlotConfig> slots_;
    Clock clock_;
    FileBook book_;
    bool started_ = false;
    bool sent_initial_keyframe_ = false;
    std::vector<Level> last_sent_bids_;
    std::vector<Level> last_sent_asks_;
    std::unordered_set<std::uint32_t> resync_slots_;
};

}  // namespace population
