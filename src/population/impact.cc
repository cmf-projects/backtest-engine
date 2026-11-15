#include "population/impact.hpp"

#include "population/price_ticks.hpp"

#include <algorithm>
#include <cmath>

namespace population {
namespace {

// Bids are best-first (descending), asks are best-first (ascending).
[[nodiscard]] bool SideHasTick(const std::vector<Level>& levels, long long price_ticks,
                               double tick_size, bool descending) {
    const auto it = descending
                        ? std::lower_bound(levels.begin(), levels.end(), price_ticks,
                                           [&](const Level& level, long long ticks) {
                                               return PriceToTicks(level.price, tick_size) > ticks;
                                           })
                        : std::lower_bound(levels.begin(), levels.end(), price_ticks,
                                           [&](const Level& level, long long ticks) {
                                               return PriceToTicks(level.price, tick_size) < ticks;
                                           });
    return it != levels.end() && PriceToTicks(it->price, tick_size) == price_ticks;
}

}  // namespace

void ImpactState::Recover(const std::vector<Level>& file_bids, const std::vector<Level>& file_asks,
                          std::uint64_t t_new_ns, double tick_size) {
    if (!enabled() || levels_.empty()) {
        return;
    }

    std::size_t i = 0;
    while (i < levels_.size()) {
        EatenLevel& st = levels_[i];
        const bool alive = (st.side == Side::Buy)
                               ? SideHasTick(file_bids, st.price_ticks, tick_size, true)
                               : SideHasTick(file_asks, st.price_ticks, tick_size, false);
        if (!alive) {
            levels_[i] = levels_.back();
            levels_.pop_back();
            continue;
        }

        if (t_new_ns > st.t_last_ns) {
            const double dt = static_cast<double>(t_new_ns - st.t_last_ns);
            st.eaten *= std::exp(-lambda_ * dt);
            st.t_last_ns = t_new_ns;
            if (st.eaten < 1e-12) {
                levels_[i] = levels_.back();
                levels_.pop_back();
                continue;
            }
        }
        ++i;
    }
}

ImpactState::EatenLevel* ImpactState::Find(Side side, long long price_ticks) {
    for (EatenLevel& level : levels_) {
        if (level.side == side && level.price_ticks == price_ticks) {
            return &level;
        }
    }
    return nullptr;
}

const ImpactState::EatenLevel* ImpactState::Find(Side side, long long price_ticks) const {
    for (const EatenLevel& level : levels_) {
        if (level.side == side && level.price_ticks == price_ticks) {
            return &level;
        }
    }
    return nullptr;
}

void ImpactState::ApplySide(Side side, std::vector<Level>& levels, double tick_size) const {
    for (Level& level : levels) {
        const long long ticks = PriceToTicks(level.price, tick_size);
        const EatenLevel* eaten = Find(side, ticks);
        if (eaten == nullptr) {
            continue;
        }
        const double left = static_cast<double>(level.qty) - eaten->eaten;
        level.qty = left > 0.0 ? static_cast<std::uint64_t>(left) : 0;
    }
    levels.erase(std::remove_if(levels.begin(), levels.end(),
                                [](const Level& level) { return level.empty(); }),
                 levels.end());
}

void ImpactState::ApplyTo(std::vector<Level>& bids, std::vector<Level>& asks,
                          double tick_size) const {
    if (!enabled() || levels_.empty()) {
        return;
    }
    ApplySide(Side::Buy, bids, tick_size);
    ApplySide(Side::Sell, asks, tick_size);
}

void ImpactState::OnFill(Side order_side, long long price_ticks, std::uint64_t qty,
                         std::uint64_t t_ns) {
    if (!enabled() || qty == 0) {
        return;
    }
    const Side book_side = (order_side == Side::Buy) ? Side::Sell : Side::Buy;
    if (levels_.capacity() < kMaxEatenLevels) {
        levels_.reserve(kMaxEatenLevels);
    }
    if (EatenLevel* st = Find(book_side, price_ticks)) {
        st->eaten += static_cast<double>(qty);
        st->t_last_ns = t_ns;
        return;
    }
    levels_.push_back(EatenLevel{book_side, price_ticks, static_cast<double>(qty), t_ns});
}

double ImpactState::EatenAt(Side side, long long price_ticks) const {
    const EatenLevel* eaten = Find(side, price_ticks);
    return eaten == nullptr ? 0.0 : eaten->eaten;
}

}  // namespace population
