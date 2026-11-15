#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace population {

// Population logical clock (`pop_ts`). Pull-advanced by the Population Service
// after each Tick/Orders round (docs/POPULATION_TZ.md §5.4).
//
// Does not read the dataset: the caller supplies the next event timestamp
// (after tape_stride folding). Match/Δ/md_lag live elsewhere.
class Clock {
public:
    [[nodiscard]] bool started() const noexcept { return started_; }

    // Undefined if !started().
    [[nodiscard]] std::uint64_t pop_ts_ns() const {
        if (!started_) {
            throw std::logic_error("population::Clock: not started");
        }
        return pop_ts_ns_;
    }

    // Pin pop_ts to the first dataset event. Call once.
    void Start(std::uint64_t first_event_ts_ns);

    // After client Advance: move to the next event ts (must be strictly greater).
    void AdvanceTo(std::uint64_t next_event_ts_ns);

private:
    bool started_ = false;
    std::uint64_t pop_ts_ns_ = 0;
};

}  // namespace population
