#include "population/merged_tape.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace population {

MergedTape::MergedTape(std::vector<MergedLeg> legs) {
    if (legs.empty()) {
        throw std::invalid_argument("MergedTape: need at least one leg");
    }
    if (legs.size() > 3) {
        throw std::invalid_argument("MergedTape: at most 3 legs");
    }

    std::unordered_set<std::string> seen;
    legs_.reserve(legs.size());
    for (auto& in : legs) {
        if (in.component_id.empty()) {
            throw std::invalid_argument("MergedTape: empty component_id");
        }
        if (in.tape == nullptr) {
            throw std::invalid_argument("MergedTape: null tape");
        }
        if (!seen.insert(in.component_id).second) {
            throw std::invalid_argument("MergedTape: duplicate component_id");
        }

        LegState st;
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
        legs_.push_back(std::move(st));
    }

    std::sort(legs_.begin(), legs_.end(),
              [](const LegState& a, const LegState& b) { return a.component_id < b.component_id; });

    start_ts_ = *MinPendingTs();
}

std::optional<std::uint64_t> MergedTape::MinPendingTs() const {
    std::optional<std::uint64_t> best;
    for (const auto& leg : legs_) {
        if (!leg.pending.has_value()) {
            continue;
        }
        if (!best.has_value() || leg.pending->ts_ns < *best) {
            best = leg.pending->ts_ns;
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
    step.components.reserve(legs_.size());

    for (auto& leg : legs_) {
        if (leg.pending.has_value() && leg.pending->ts_ns == t) {
            if (leg.last.has_value() && leg.pending->ts_ns <= leg.last->ts_ns) {
                throw std::logic_error("MergedTape: non-increasing ts on leg " + leg.component_id);
            }
            leg.last = std::move(*leg.pending);
            leg.pending = leg.tape->Next();
            if (leg.pending.has_value() && leg.pending->ts_ns <= leg.last->ts_ns) {
                throw std::logic_error("MergedTape: non-increasing pending ts on leg " +
                                       leg.component_id);
            }
            step.components.push_back(
                ComponentSnap{.component_id = leg.component_id, .snap = *leg.last, .updated = true});
            continue;
        }

        if (leg.last.has_value()) {
            step.components.push_back(ComponentSnap{
                .component_id = leg.component_id, .snap = *leg.last, .updated = false});
        }
        // else: leg not started yet (first event still in the future) — omit
    }

    return step;
}

}  // namespace population
