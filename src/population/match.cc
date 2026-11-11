#include "population/match.hpp"

#include <algorithm>

namespace population {

bool CanFullyFill(Side side, long long price_ticks, std::uint64_t qty,
                  const std::vector<Level>& opposite, double tick_size) {
    std::uint64_t need = qty;
    for (const Level& level : opposite) {
        if (need == 0) {
            break;
        }
        if (level.qty == 0) {
            continue;
        }
        const long long level_ticks = PriceToTicks(level.price, tick_size);
        const bool crosses = (side == Side::Buy) ? (price_ticks >= level_ticks)
                                                 : (price_ticks <= level_ticks);
        if (!crosses) {
            break;
        }
        if (level.qty >= need) {
            return true;
        }
        need -= level.qty;
    }
    return need == 0;
}

void MatchOne(WorkingOrder& working, std::vector<Level>& opposite, double tick_size,
              std::vector<Trade>& trades) {
    while (working.leaves > 0 && !opposite.empty()) {
        Level& touch = opposite.front();
        if (touch.qty == 0) {
            opposite.erase(opposite.begin());
            continue;
        }

        const long long touch_ticks = PriceToTicks(touch.price, tick_size);
        const bool crosses = (working.side == Side::Buy) ? (working.price_ticks >= touch_ticks)
                                                         : (working.price_ticks <= touch_ticks);
        if (!crosses) {
            break;
        }

        const std::uint64_t qty = std::min(working.leaves, touch.qty);
        working.leaves -= qty;
        touch.qty -= qty;

        Trade fill;
        fill.client_order_id = working.client_order_id;
        fill.exec_type = ExecType::Fill;
        fill.component_id = working.component_id;
        fill.price = static_cast<double>(touch_ticks) * tick_size;
        fill.qty = qty;
        fill.leaves = working.leaves;
        trades.push_back(std::move(fill));

        if (touch.qty == 0) {
            opposite.erase(opposite.begin());
        }
    }
}

}  // namespace population
