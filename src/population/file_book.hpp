#pragma once

#include "population/clock.hpp"
#include "population/tape.hpp"
#include "population/tape_window.hpp"
#include "population/types.hpp"

#include <optional>
#include <vector>

namespace population {

// One-component file book cursor: applies tape snapshots, drives Clock,
// and appends each applied snap into TapeWindow (history for md_lag later).
class FileBook {
public:
    explicit FileBook(TapePtr tape);

    // Consume first event, Start the clock, materialize bids/asks, Append window.
    void Start(Clock& clock);

    // Consume next event, AdvanceTo its ts, replace bids/asks, Append window.
    // Returns false at EOF (book, clock, window unchanged).
    [[nodiscard]] bool Advance(Clock& clock);

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }

    [[nodiscard]] const SnapshotEvent& current() const;
    [[nodiscard]] const std::vector<Level>& bids() const { return current().bids; }
    [[nodiscard]] const std::vector<Level>& asks() const { return current().asks; }

    [[nodiscard]] const TapeWindow& window() const noexcept { return window_; }
    [[nodiscard]] TapeWindow& window() noexcept { return window_; }

private:
    TapePtr tape_;
    std::optional<SnapshotEvent> current_;
    TapeWindow window_;
    bool started_ = false;
    bool ended_ = false;
};

}  // namespace population
