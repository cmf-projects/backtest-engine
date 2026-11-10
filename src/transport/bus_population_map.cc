#include "transport/bus_population_map.hpp"

#include <cmath>
#include <cstdio>

namespace transport {
namespace {

[[nodiscard]] population::Side ToSide(BusSide side) {
    switch (side) {
        case BusSide::Buy:
            return population::Side::Buy;
        case BusSide::Sell:
            return population::Side::Sell;
        case BusSide::Unspecified:
            return population::Side::Unknown;
    }
    return population::Side::Unknown;
}

[[nodiscard]] population::TimeInForce ToTif(BusTimeInForce tif) {
    switch (tif) {
        case BusTimeInForce::Day:
            return population::TimeInForce::Day;
        case BusTimeInForce::Ioc:
            return population::TimeInForce::Ioc;
        case BusTimeInForce::Fok:
            return population::TimeInForce::Fok;
        case BusTimeInForce::Unspecified:
            return population::TimeInForce::Unknown;
    }
    return population::TimeInForce::Unknown;
}

[[nodiscard]] BusBookPatchKind ToBusPatchKind(population::BookPatchKind kind) {
    switch (kind) {
        case population::BookPatchKind::Keyframe:
            return BusBookPatchKind::Keyframe;
        case population::BookPatchKind::Diff:
            return BusBookPatchKind::Diff;
    }
    return BusBookPatchKind::Unspecified;
}

[[nodiscard]] BusExecType ToBusExec(population::ExecType exec) {
    switch (exec) {
        case population::ExecType::Fill:
            return BusExecType::Fill;
        case population::ExecType::Cancel:
            return BusExecType::Cancel;
        case population::ExecType::Unknown:
            return BusExecType::Unspecified;
    }
    return BusExecType::Unspecified;
}

void AppendLevels(std::vector<BusBookLevel>& out, const std::vector<population::Level>& in) {
    out.reserve(in.size());
    for (const auto& lvl : in) {
        out.push_back(BusBookLevel{.price = FormatDecimal(lvl.price), .qty = lvl.qty});
    }
}

}  // namespace

bool ParseDecimal(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t idx = 0;
        const double v = std::stod(text, &idx);
        if (idx != text.size() || !std::isfinite(v)) {
            return false;
        }
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

std::string FormatDecimal(double value) {
    char buf[64];
    // Enough digits to round-trip typical market prices; wire is decimal text.
    const int n = std::snprintf(buf, sizeof(buf), "%.15g", value);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
        return {};
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

population::SlotConfig ToSlotConfig(const BusSlotConfig& in) {
    population::SlotConfig out;
    out.slot_id = in.slot_id;
    out.md_lag_ns = in.md_lag_ns;
    out.delta_ns = in.delta_ns;
    if (!ParseDecimal(in.impact_lambda, out.impact_lambda)) {
        out.impact_lambda = 0.0;
    }
    return out;
}

std::vector<population::SlotConfig> ToSlotConfigs(const std::vector<BusSlotConfig>& in) {
    std::vector<population::SlotConfig> out;
    out.reserve(in.size());
    for (const auto& s : in) {
        out.push_back(ToSlotConfig(s));
    }
    return out;
}

population::OrderBatch ToOrderBatch(const BusOrderBatch& in) {
    population::OrderBatch out;
    out.orders.reserve(in.orders.size());
    for (const auto& o : in.orders) {
        population::NewOrder n;
        n.slot_id = o.slot_id;
        n.client_order_id = o.client_order_id;
        n.component_id = o.component_id;
        n.side = ToSide(o.side);
        n.tif = ToTif(o.tif);
        n.qty = o.qty;
        if (!ParseDecimal(o.price, n.price)) {
            n.price = 0.0;
        }
        out.orders.push_back(std::move(n));
    }
    out.cancels.reserve(in.cancels.size());
    for (const auto& c : in.cancels) {
        out.cancels.push_back(population::CancelOrder{
            .slot_id = c.slot_id,
            .client_order_id = c.client_order_id,
        });
    }
    return out;
}

BusTick ToBusTick(const population::TickView& in) {
    BusTick out;
    out.pop_ts_ns = in.pop_ts_ns;
    out.books.reserve(in.books.size());
    for (const auto& book : in.books) {
        BusBookUpdate u;
        u.component_id = book.component_id;
        u.kind = ToBusPatchKind(book.kind);
        AppendLevels(u.bids, book.bids);
        AppendLevels(u.asks, book.asks);
        out.books.push_back(std::move(u));
    }
    out.slots.reserve(in.slots.size());
    for (const auto& slot : in.slots) {
        BusSlotTick st;
        st.slot_id = slot.slot_id;
        st.books.reserve(slot.books.size());
        for (const auto& book : slot.books) {
            BusBookUpdate u;
            u.component_id = book.component_id;
            u.kind = ToBusPatchKind(book.kind);
            AppendLevels(u.bids, book.bids);
            AppendLevels(u.asks, book.asks);
            st.books.push_back(std::move(u));
        }
        st.trades.reserve(slot.trades.size());
        for (const auto& trade : slot.trades) {
            st.trades.push_back(BusTrade{
                .client_order_id = trade.client_order_id,
                .exec_type = ToBusExec(trade.exec_type),
                .component_id = trade.component_id,
                .price = FormatDecimal(trade.price),
                .qty = trade.qty,
                .leaves = trade.leaves,
            });
        }
        out.slots.push_back(std::move(st));
    }
    return out;
}

}  // namespace transport
