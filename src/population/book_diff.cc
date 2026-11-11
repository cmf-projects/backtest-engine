#include "population/book_diff.hpp"

#include "population/price_ticks.hpp"

namespace population {
namespace {

[[nodiscard]] const Level* FindByTicks(const std::vector<Level>& levels, long long ticks,
                                       double tick_size) {
    for (const Level& level : levels) {
        if (PriceToTicks(level.price, tick_size) == ticks) {
            return &level;
        }
    }
    return nullptr;
}

void DiffSide(const std::vector<Level>& before, const std::vector<Level>& after, double tick_size,
              std::vector<Level>& out) {
    out.clear();
    if (out.capacity() < before.size() + after.size()) {
        out.reserve(before.size() + after.size());
    }

    for (const Level& level : after) {
        const long long ticks = PriceToTicks(level.price, tick_size);
        const Level* prev = FindByTicks(before, ticks, tick_size);
        if (prev == nullptr || prev->qty != level.qty) {
            out.push_back(Level{level.price, level.qty});
        }
    }
    for (const Level& level : before) {
        const long long ticks = PriceToTicks(level.price, tick_size);
        if (FindByTicks(after, ticks, tick_size) == nullptr) {
            out.push_back(Level{level.price, 0});
        }
    }
}

}  // namespace

void DiffBooksInto(const std::vector<Level>& before_bids, const std::vector<Level>& before_asks,
                   const std::vector<Level>& after_bids, const std::vector<Level>& after_asks,
                   double tick_size, BookDiffResult& out) {
    DiffSide(before_bids, after_bids, tick_size, out.bids);
    DiffSide(before_asks, after_asks, tick_size, out.asks);
}

BookDiffResult DiffBooks(const std::vector<Level>& before_bids,
                         const std::vector<Level>& before_asks,
                         const std::vector<Level>& after_bids,
                         const std::vector<Level>& after_asks, double tick_size) {
    BookDiffResult result;
    DiffBooksInto(before_bids, before_asks, after_bids, after_asks, tick_size, result);
    return result;
}

}  // namespace population
