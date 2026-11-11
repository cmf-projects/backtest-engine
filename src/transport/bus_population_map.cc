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

void AppendLevelsInto(std::vector<BusBookLevel>& out, const std::vector<population::Level>& in) {
    if (out.size() != in.size()) {
        out.resize(in.size());
    }
    for (std::size_t i = 0; i < in.size(); ++i) {
        FormatDecimalInto(in[i].price, out[i].price);
        out[i].qty = in[i].qty;
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

void FormatDecimalInto(double value, std::string& out) {
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), "%.15g", value);
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
        out.clear();
        return;
    }
    out.assign(buf, static_cast<std::size_t>(n));
}

std::string FormatDecimal(double value) {
    std::string out;
    FormatDecimalInto(value, out);
    return out;
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

void ToOrderBatchInto(const BusOrderBatch& in, population::OrderBatch& out) {
    if (out.orders.size() != in.orders.size()) {
        out.orders.resize(in.orders.size());
    }
    for (std::size_t i = 0; i < in.orders.size(); ++i) {
        const auto& o = in.orders[i];
        population::NewOrder& n = out.orders[i];
        n.slot_id = o.slot_id;
        n.client_order_id = o.client_order_id;
        n.component_id = o.component_id;
        n.side = ToSide(o.side);
        n.tif = ToTif(o.tif);
        n.qty = o.qty;
        if (!ParseDecimal(o.price, n.price)) {
            n.price = 0.0;
        }
    }
    if (out.cancels.size() != in.cancels.size()) {
        out.cancels.resize(in.cancels.size());
    }
    for (std::size_t i = 0; i < in.cancels.size(); ++i) {
        out.cancels[i].slot_id = in.cancels[i].slot_id;
        out.cancels[i].client_order_id = in.cancels[i].client_order_id;
    }
}

population::OrderBatch ToOrderBatch(const BusOrderBatch& in) {
    population::OrderBatch out;
    ToOrderBatchInto(in, out);
    return out;
}

void ToBusTickInto(const population::TickView& in, BusTick& out) {
    out.pop_ts_ns = in.pop_ts_ns;
    if (out.books.size() != in.books.size()) {
        out.books.resize(in.books.size());
    }
    for (std::size_t i = 0; i < in.books.size(); ++i) {
        const auto& book = in.books[i];
        BusBookUpdate& u = out.books[i];
        u.component_id = book.component_id;
        u.kind = ToBusPatchKind(book.kind);
        AppendLevelsInto(u.bids, book.bids);
        AppendLevelsInto(u.asks, book.asks);
    }
    if (out.slots.size() != in.slots.size()) {
        out.slots.resize(in.slots.size());
    }
    for (std::size_t si = 0; si < in.slots.size(); ++si) {
        const auto& slot = in.slots[si];
        BusSlotTick& st = out.slots[si];
        st.slot_id = slot.slot_id;
        if (st.books.size() != slot.books.size()) {
            st.books.resize(slot.books.size());
        }
        for (std::size_t bi = 0; bi < slot.books.size(); ++bi) {
            const auto& book = slot.books[bi];
            BusBookUpdate& u = st.books[bi];
            u.component_id = book.component_id;
            u.kind = ToBusPatchKind(book.kind);
            AppendLevelsInto(u.bids, book.bids);
            AppendLevelsInto(u.asks, book.asks);
        }
        if (st.trades.size() != slot.trades.size()) {
            st.trades.resize(slot.trades.size());
        }
        for (std::size_t ti = 0; ti < slot.trades.size(); ++ti) {
            const auto& trade = slot.trades[ti];
            BusTrade& tr = st.trades[ti];
            tr.client_order_id = trade.client_order_id;
            tr.exec_type = ToBusExec(trade.exec_type);
            tr.component_id = trade.component_id;
            FormatDecimalInto(trade.price, tr.price);
            tr.qty = trade.qty;
            tr.leaves = trade.leaves;
        }
    }
}

BusTick ToBusTick(const population::TickView& in) {
    BusTick out;
    ToBusTickInto(in, out);
    return out;
}

}  // namespace transport
