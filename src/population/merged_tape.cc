#include "population/merged_tape.hpp"

#include "population/snapshot_assign.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace population {
namespace {

void FillComponentSnap(ComponentSnap& dst, const std::string& component_id,
                       const SnapshotEvent* src, bool updated) {
    dst.component_id = component_id;
    dst.snap = src;
    dst.updated = updated;
}

}  // namespace

MergedTape::MergedTape(std::vector<MergedComponent> components) {
    if (components.empty()) {
        throw std::invalid_argument("MergedTape: need at least one component");
    }
    if (components.size() > 3) {
        throw std::invalid_argument("MergedTape: at most 3 components");
    }

    std::unordered_set<std::string> seen;
    components_.reserve(components.size());
    for (auto& in : components) {
        if (in.component_id.empty()) {
            throw std::invalid_argument("MergedTape: empty component_id");
        }
        if (in.tape == nullptr) {
            throw std::invalid_argument("MergedTape: null tape");
        }
        if (!seen.insert(in.component_id).second) {
            throw std::invalid_argument("MergedTape: duplicate component_id");
        }

        ComponentState st;
        st.component_id = std::move(in.component_id);
        st.tape = std::move(in.tape);
        st.pending.bids.reserve(kMaxLevelsPerSide);
        st.pending.asks.reserve(kMaxLevelsPerSide);
        st.last.bids.reserve(kMaxLevelsPerSide);
        st.last.asks.reserve(kMaxLevelsPerSide);
        st.has_pending = st.tape->NextInto(st.pending);
        if (!st.has_pending) {
            throw std::invalid_argument("MergedTape: tape yielded no first event (" +
                                        st.component_id + ")");
        }
        if (st.pending.ts_ns == 0) {
            throw std::invalid_argument("MergedTape: first event ts_ns must be non-zero");
        }
        components_.push_back(std::move(st));
    }

    std::sort(components_.begin(), components_.end(),
              [](const ComponentState& a, const ComponentState& b) {
                  return a.component_id < b.component_id;
              });

    start_ts_ = *MinPendingTs();
    step_.components.reserve(components_.size());
}

std::optional<std::uint64_t> MergedTape::MinPendingTs() const {
    std::optional<std::uint64_t> best;
    for (const auto& c : components_) {
        if (!c.has_pending) {
            continue;
        }
        if (!best.has_value() || c.pending.ts_ns < *best) {
            best = c.pending.ts_ns;
        }
    }
    return best;
}

bool MergedTape::NextInto(MergedStep& out) {
    const auto t_opt = MinPendingTs();
    if (!t_opt.has_value()) {
        return false;
    }
    const std::uint64_t t = *t_opt;
    out.ts_ns = t;

    std::size_t n = 0;
    for (const auto& c : components_) {
        if (c.has_pending && c.pending.ts_ns == t) {
            ++n;
        } else if (c.has_last) {
            ++n;
        }
    }
    if (out.components.size() != n) {
        out.components.resize(n);
    }

    std::size_t i = 0;
    for (auto& c : components_) {
        if (c.has_pending && c.pending.ts_ns == t) {
            if (c.has_last && c.pending.ts_ns <= c.last.ts_ns) {
                throw std::logic_error("MergedTape: non-increasing ts on component " +
                                       c.component_id);
            }
            // Promote pending → last without freeing either side's level capacity.
            AssignSnapshot(c.last, std::move(c.pending));
            c.has_last = true;
            if (c.pending.bids.capacity() < kMaxLevelsPerSide) {
                c.pending.bids.reserve(kMaxLevelsPerSide);
            }
            if (c.pending.asks.capacity() < kMaxLevelsPerSide) {
                c.pending.asks.reserve(kMaxLevelsPerSide);
            }
            c.has_pending = c.tape->NextInto(c.pending);
            if (c.has_pending && c.pending.ts_ns <= c.last.ts_ns) {
                throw std::logic_error("MergedTape: non-increasing pending ts on component " +
                                       c.component_id);
            }
            FillComponentSnap(out.components[i++], c.component_id, &c.last, true);
            continue;
        }

        if (c.has_last) {
            FillComponentSnap(out.components[i++], c.component_id, &c.last, false);
        }
    }
    return true;
}

std::optional<MergedStep> MergedTape::Next() {
    if (!NextInto(step_)) {
        return std::nullopt;
    }
    return step_;
}

}  // namespace population
