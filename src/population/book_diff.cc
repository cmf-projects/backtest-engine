#include "population/book_diff.hpp"

#include "population/price_ticks.hpp"

#include <map>

namespace population {
namespace {

void DiffSide(const std::vector<Level>& before, const std::vector<Level>& after, double tick_size,
              std::vector<Level>& out) {
    std::map<long long, Level> prev;
    std::map<long long, Level> next;
    for (const Level& level : before) {
        prev[PriceToTicks(level.price, tick_size)] = level;
    }
    for (const Level& level : after) {
        next[PriceToTicks(level.price, tick_size)] = level;
    }

    for (const auto& [ticks, level] : next) {
        const auto it = prev.find(ticks);
        if (it == prev.end() || it->second.qty != level.qty) {
            out.push_back(Level{level.price, level.qty});
        }
    }
    for (const auto& [ticks, level] : prev) {
        if (next.find(ticks) == next.end()) {
            out.push_back(Level{level.price, 0});
        }
    }
}

}  // namespace

BookDiffResult DiffBooks(const std::vector<Level>& before_bids,
                         const std::vector<Level>& before_asks,
                         const std::vector<Level>& after_bids,
                         const std::vector<Level>& after_asks, double tick_size) {
    BookDiffResult result;
    DiffSide(before_bids, after_bids, tick_size, result.bids);
    DiffSide(before_asks, after_asks, tick_size, result.asks);
    return result;
}

}  // namespace population
