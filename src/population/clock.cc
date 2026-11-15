#include "population/clock.hpp"

namespace population {

void Clock::Start(std::uint64_t first_event_ts_ns) {
    if (started_) {
        throw std::logic_error("population::Clock: already started");
    }
    if (first_event_ts_ns == 0) {
        throw std::invalid_argument("population::Clock: first_event_ts_ns must be non-zero");
    }
    pop_ts_ns_ = first_event_ts_ns;
    started_ = true;
}

void Clock::AdvanceTo(std::uint64_t next_event_ts_ns) {
    if (!started_) {
        throw std::logic_error("population::Clock: AdvanceTo before Start");
    }
    if (next_event_ts_ns <= pop_ts_ns_) {
        throw std::invalid_argument(
            "population::Clock: AdvanceTo requires next_ts > pop_ts (got " +
            std::to_string(next_event_ts_ns) + " <= " + std::to_string(pop_ts_ns_) + ")");
    }
    pop_ts_ns_ = next_event_ts_ns;
}

}  // namespace population
