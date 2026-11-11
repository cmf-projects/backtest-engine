#pragma once

#include "population/impact.hpp"
#include "population/order.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <vector>

namespace population {

// Per-slot working book + due match against a private file-book copy.
class SlotOrders {
public:
    [[nodiscard]] OrderResult Accept(WorkingOrder order);

    [[nodiscard]] OrderResult Cancel(std::uint64_t client_order_id);

    // Match orders with effective_ts <= pop_ts_ns against mutable level copies
    // (already effective if caller applied impact). Does not mutate FileBook.
    // Only orders with matching `component_id` are considered.
    // Appends fills/cancels into `trades`. Fills update `impact` when non-null.
    void MatchDue(std::uint64_t pop_ts_ns, std::vector<Level>& bids, std::vector<Level>& asks,
                  double tick_size, ImpactState* impact, const std::string& component_id,
                  std::vector<Trade>& trades);

    [[nodiscard]] std::size_t LiveCount() const;
    [[nodiscard]] const std::vector<WorkingOrder>& orders() const noexcept { return orders_; }

    [[nodiscard]] const WorkingOrder* FindLive(std::uint64_t client_order_id) const;

private:
    void Reap();

    std::vector<WorkingOrder> orders_;
};

}  // namespace population
