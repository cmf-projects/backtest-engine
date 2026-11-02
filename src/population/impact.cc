#include "population/impact.hpp"

#include "population/match.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace population {
namespace {

[[nodiscard]] std::unordered_set<long long> PresentTicks(const std::vector<Level>& levels,
                                                         double tick_size) {
    std::unordered_set<long long> out;
    out.reserve(levels.size());
    for (const Level& level : levels) {
        out.insert(PriceToTicks(level.price, tick_size));
    }
    return out;
}

}  // namespace

void ImpactState::Recover(const std::vector<Level>& file_bids, const std::vector<Level>& file_asks,
                          std::uint64_t t_new_ns, double tick_size) {
    if (!enabled() || levels_.empty()) {
        return;
    }

    const auto bid_ticks = PresentTicks(file_bids, tick_size);
    const auto ask_ticks = PresentTicks(file_asks, tick_size);

    for (auto it = levels_.begin(); it != levels_.end();) {
        const bool alive = (it->first.side == Side::Buy)
                               ? bid_ticks.count(it->first.price_ticks) > 0
                               : ask_ticks.count(it->first.price_ticks) > 0;
        if (!alive) {
            it = levels_.erase(it);
            continue;
        }

        LevelState& st = it->second;
        if (t_new_ns > st.t_last_ns) {
            const double dt = static_cast<double>(t_new_ns - st.t_last_ns);
            st.eaten *= std::exp(-lambda_ * dt);
            st.t_last_ns = t_new_ns;
            if (st.eaten < 1e-12) {
                it = levels_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void ImpactState::ApplyTo(std::vector<Level>& bids, std::vector<Level>& asks,
                          double tick_size) const {
    if (!enabled() || levels_.empty()) {
        return;
    }

    auto apply_side = [&](Side side, std::vector<Level>& levels) {
        for (Level& level : levels) {
            const long long ticks = PriceToTicks(level.price, tick_size);
            const auto it = levels_.find(ImpactKey{side, ticks});
            if (it == levels_.end()) {
                continue;
            }
            const double left = static_cast<double>(level.qty) - it->second.eaten;
            level.qty = left > 0.0 ? static_cast<std::uint64_t>(left) : 0;
        }
        levels.erase(std::remove_if(levels.begin(), levels.end(),
                                    [](const Level& l) { return l.qty == 0; }),
                     levels.end());
    };

    apply_side(Side::Buy, bids);
    apply_side(Side::Sell, asks);
}

void ImpactState::OnFill(Side order_side, long long price_ticks, std::uint64_t qty,
                         std::uint64_t t_ns) {
    if (!enabled() || qty == 0) {
        return;
    }
    const Side book_side = (order_side == Side::Buy) ? Side::Sell : Side::Buy;
    LevelState& st = levels_[ImpactKey{book_side, price_ticks}];
    st.eaten += static_cast<double>(qty);
    st.t_last_ns = t_ns;
}

double ImpactState::EatenAt(Side side, long long price_ticks) const {
    const auto it = levels_.find(ImpactKey{side, price_ticks});
    if (it == levels_.end()) {
        return 0.0;
    }
    return it->second.eaten;
}

}  // namespace population
