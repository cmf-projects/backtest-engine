#include "population/slot_orders.hpp"

#include "population/match.hpp"
#include "population/price_ticks.hpp"

#include <algorithm>

namespace population {

OrderResult SlotOrders::Accept(WorkingOrder order) {
    if (FindLive(order.client_order_id) != nullptr) {
        return OrderResult::DuplicateOrderId;
    }
    orders_.push_back(std::move(order));
    return OrderResult::Ok;
}

OrderResult SlotOrders::Cancel(std::uint64_t client_order_id) {
    for (WorkingOrder& order : orders_) {
        if (order.client_order_id == client_order_id && order.leaves > 0) {
            order.leaves = 0;
            Reap();
            return OrderResult::Ok;
        }
    }
    return OrderResult::NoSuchOrder;
}

void SlotOrders::MatchDue(std::uint64_t pop_ts_ns, std::vector<Level>& bids,
                          std::vector<Level>& asks, double tick_size, ImpactState* impact,
                          const std::string& component_id, std::vector<Trade>& trades) {
    for (WorkingOrder& working : orders_) {
        if (working.leaves == 0 || working.effective_ts_ns > pop_ts_ns ||
            working.component_id != component_id) {
            continue;
        }

        std::vector<Level>& opposite = (working.side == Side::Buy) ? asks : bids;

        if (working.tif == TimeInForce::Fok) {
            if (!CanFullyFill(working.side, working.price_ticks, working.leaves, opposite,
                             tick_size)) {
                Trade cancel;
                cancel.client_order_id = working.client_order_id;
                cancel.exec_type = ExecType::Cancel;
                cancel.component_id = working.component_id;
                cancel.leaves = 0;
                trades.push_back(std::move(cancel));
                working.leaves = 0;
                continue;
            }
        }

        const std::size_t trades_before = trades.size();
        MatchOne(working, opposite, tick_size, trades);
        if (impact != nullptr) {
            for (std::size_t i = trades_before; i < trades.size(); ++i) {
                if (trades[i].exec_type != ExecType::Fill) {
                    continue;
                }
                impact->OnFill(working.side, PriceToTicks(trades[i].price, tick_size),
                               trades[i].qty, pop_ts_ns);
            }
        }

        if (working.tif == TimeInForce::Ioc && working.leaves > 0) {
            Trade cancel;
            cancel.client_order_id = working.client_order_id;
            cancel.exec_type = ExecType::Cancel;
            cancel.component_id = working.component_id;
            cancel.leaves = 0;
            trades.push_back(std::move(cancel));
            working.leaves = 0;
        }
    }

    Reap();
}

std::size_t SlotOrders::LiveCount() const {
    return static_cast<std::size_t>(
        std::count_if(orders_.begin(), orders_.end(),
                      [](const WorkingOrder& o) { return o.leaves > 0; }));
}

const WorkingOrder* SlotOrders::FindLive(std::uint64_t client_order_id) const {
    for (const WorkingOrder& order : orders_) {
        if (order.client_order_id == client_order_id && order.leaves > 0) {
            return &order;
        }
    }
    return nullptr;
}

void SlotOrders::Reap() {
    orders_.erase(std::remove_if(orders_.begin(), orders_.end(),
                                 [](const WorkingOrder& o) { return o.leaves == 0; }),
                  orders_.end());
}

}  // namespace population
