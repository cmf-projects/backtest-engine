#pragma once

#include "population/clock.hpp"
#include "population/tape.hpp"
#include "population/types.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace population {

// One-component file book cursor: applies tape snapshots and drives Clock.
// P0 stand-in for TapeWindow on a single Component (no BookDelta chain yet).
class FileBook {
public:
    explicit FileBook(TapePtr tape);

    // Consume first event, Start the clock, materialize bids/asks.
    void Start(Clock& clock);

    // Consume next event, AdvanceTo its ts, replace bids/asks.
    // Returns false at EOF (book and clock unchanged).
    [[nodiscard]] bool Advance(Clock& clock);

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }

    [[nodiscard]] const SnapshotEvent& current() const;
    [[nodiscard]] const std::vector<Level>& bids() const { return current().bids; }
    [[nodiscard]] const std::vector<Level>& asks() const { return current().asks; }

private:
    TapePtr tape_;
    std::optional<SnapshotEvent> current_;
    bool started_ = false;
    bool ended_ = false;
};

}  // namespace population
