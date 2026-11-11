#include "population/merged_tape.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace population {

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
        st.pending = st.tape->Next();
        if (!st.pending.has_value()) {
            throw std::invalid_argument("MergedTape: tape yielded no first event (" +
                                        st.component_id + ")");
        }
        if (st.pending->ts_ns == 0) {
            throw std::invalid_argument("MergedTape: first event ts_ns must be non-zero");
        }
        components_.push_back(std::move(st));
    }

    std::sort(components_.begin(), components_.end(),
              [](const ComponentState& a, const ComponentState& b) {
                  return a.component_id < b.component_id;
              });

    start_ts_ = *MinPendingTs();
}

std::optional<std::uint64_t> MergedTape::MinPendingTs() const {
    std::optional<std::uint64_t> best;
    for (const auto& c : components_) {
        if (!c.pending.has_value()) {
            continue;
        }
        if (!best.has_value() || c.pending->ts_ns < *best) {
            best = c.pending->ts_ns;
        }
    }
    return best;
}

std::optional<MergedStep> MergedTape::Next() {
    const auto t_opt = MinPendingTs();
    if (!t_opt.has_value()) {
        return std::nullopt;
    }
    const std::uint64_t t = *t_opt;

    MergedStep step;
    step.ts_ns = t;
    step.components.reserve(components_.size());

    for (auto& c : components_) {
        if (c.pending.has_value() && c.pending->ts_ns == t) {
            if (c.last.has_value() && c.pending->ts_ns <= c.last->ts_ns) {
                throw std::logic_error("MergedTape: non-increasing ts on component " +
                                       c.component_id);
            }
            c.last = std::move(*c.pending);
            c.pending = c.tape->Next();
            if (c.pending.has_value() && c.pending->ts_ns <= c.last->ts_ns) {
                throw std::logic_error("MergedTape: non-increasing pending ts on component " +
                                       c.component_id);
            }
            step.components.push_back(
                ComponentSnap{.component_id = c.component_id, .snap = *c.last, .updated = true});
            continue;
        }

        if (c.last.has_value()) {
            step.components.push_back(ComponentSnap{
                .component_id = c.component_id, .snap = *c.last, .updated = false});
        }
        // else: component not started yet (first event still in the future) — omit
    }

    return step;
}

}  // namespace population
