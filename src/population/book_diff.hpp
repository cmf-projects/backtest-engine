#pragma once

#include "population/types.hpp"

#include <vector>

namespace population {

// Absolute-qty level patches (qty 0 = remove), same rule as CMF `I`.
struct BookDiffResult {
    std::vector<Level> bids;
    std::vector<Level> asks;

    [[nodiscard]] std::size_t change_count() const noexcept {
        return bids.size() + asks.size();
    }

    [[nodiscard]] bool empty() const noexcept { return bids.empty() && asks.empty(); }
};

// Diff `after` vs `before`. Prices compared in whole ticks.
[[nodiscard]] BookDiffResult DiffBooks(const std::vector<Level>& before_bids,
                                       const std::vector<Level>& before_asks,
                                       const std::vector<Level>& after_bids,
                                       const std::vector<Level>& after_asks,
                                       double tick_size);

void DiffBooksInto(const std::vector<Level>& before_bids, const std::vector<Level>& before_asks,
                   const std::vector<Level>& after_bids, const std::vector<Level>& after_asks,
                   double tick_size, BookDiffResult& out);

}  // namespace population
