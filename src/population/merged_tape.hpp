#pragma once

// Merge N component Tapes into one timeline by ts_ns (TZ §6; IMPL 3.1).
// On each step: all components that already had a first event appear; those
// without an event at this ts keep their last snapshot (keep-last). Components
// not yet started are omitted until their first event.

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
    // Points at MergedTape::ComponentState::last (valid until next Next/NextInto).
    const SnapshotEvent* snap = nullptr;
    bool updated = false;  // true iff this component consumed a tape event at step ts
};

struct MergedStep {
    std::uint64_t ts_ns = 0;
    std::vector<ComponentSnap> components;  // sorted by component_id
};

struct MergedComponent {
    std::string component_id;
    TapePtr tape;
};

class MergedTape {
public:
    // 1…N non-null tapes with unique non-empty component_id.
    // Each tape must already satisfy Tape contract (pending first event).
    explicit MergedTape(std::vector<MergedComponent> components);

    [[nodiscard]] std::uint64_t StartTs() const noexcept { return start_ts_; }
    [[nodiscard]] std::size_t component_count() const noexcept { return components_.size(); }

    // Next distinct ts on the merged timeline, or nullopt at EOF of all components.
    // Copies from the reused step buffer (tests / convenience).
    [[nodiscard]] std::optional<MergedStep> Next();

    // Fill `out` reusing nested SnapshotEvent capacity. Returns false at EOF.
    [[nodiscard]] bool NextInto(MergedStep& out);

private:
    struct ComponentState {
        std::string component_id;
        TapePtr tape;
        SnapshotEvent pending;
        SnapshotEvent last;
        bool has_pending = false;
        bool has_last = false;
    };

    [[nodiscard]] std::optional<std::uint64_t> MinPendingTs() const;

    std::vector<ComponentState> components_;
    MergedStep step_;  // reused by Next()
    std::uint64_t start_ts_ = 0;
};

}  // namespace population
