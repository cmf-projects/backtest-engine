#pragma once

// Merge N component Tapes into one timeline by ts_ns (TZ §6; IMPL 3.1).
// On each step: all legs that already had a first event appear; legs without
// an event at this ts keep their last snapshot (keep-last). Legs not yet
// started are omitted until their first event.

#include "population/tape.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace population {

struct ComponentSnap {
    std::string component_id;
    SnapshotEvent snap;
    bool updated = false;  // true iff this leg consumed a tape event at step ts
};

struct MergedStep {
    std::uint64_t ts_ns = 0;
    std::vector<ComponentSnap> components;  // sorted by component_id
};

struct MergedLeg {
    std::string component_id;
    TapePtr tape;
};

class MergedTape {
public:
    // 1…N non-null tapes with unique non-empty component_id.
    // Each tape must already satisfy Tape contract (pending first event).
    explicit MergedTape(std::vector<MergedLeg> legs);

    [[nodiscard]] std::uint64_t StartTs() const noexcept { return start_ts_; }
    [[nodiscard]] std::size_t leg_count() const noexcept { return legs_.size(); }

    // Next distinct ts on the merged timeline, or nullopt at EOF of all legs.
    [[nodiscard]] std::optional<MergedStep> Next();

private:
    struct LegState {
        std::string component_id;
        TapePtr tape;
        std::optional<SnapshotEvent> pending;
        std::optional<SnapshotEvent> last;
    };

    [[nodiscard]] std::optional<std::uint64_t> MinPendingTs() const;

    std::vector<LegState> legs_;
    std::uint64_t start_ts_ = 0;
};

}  // namespace population
