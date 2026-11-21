#include "population_venue/log/log.hpp"
#include "orders.hpp"

using transport::BusOrder;
using transport::BusSide;
using transport::BusTimeInForce;

namespace client {
namespace {

[[nodiscard]] RememberedBook& FindBook(std::vector<RememberedBook>& memory, const std::string& id) {
    for (RememberedBook& book : memory) {
        if (book.component_id == id) {
            return book;
        }
    }
    memory.push_back(RememberedBook{.component_id = id});
    return memory.back();
}

[[nodiscard]] bool PriceLess(const transport::BusPrice& a, const transport::BusPrice& b) noexcept {
    return a.whole < b.whole || (a.whole == b.whole && a.frac < b.frac);
}

// Keyframe books are best-first: the first live level is the touch.
// A diff may list a deep level after the touch. A worse price does not replace it.
void ApplySide(bool& has, transport::BusPrice& price, std::uint64_t& qty,
               const std::vector<transport::BusBookLevel>& levels, bool replace,
               bool higher_is_better) {
    if (replace) {
        has = false;
        qty = 0;
        for (const transport::BusBookLevel& level : levels) {
            if (level.qty == 0) {
                continue;
            }
            has = true;
            price = level.price;
            qty = level.qty;
            return;
        }
        return;
    }
    for (const transport::BusBookLevel& level : levels) {
        if (level.qty == 0) {
            if (has && price == level.price) {
                has = false;
                qty = 0;
            }
            continue;
        }
        if (!has || price == level.price) {
            has = true;
            price = level.price;
            qty = level.qty;
            continue;
        }
        const bool better =
            higher_is_better ? PriceLess(price, level.price) : PriceLess(level.price, price);
        if (better) {
            price = level.price;
            qty = level.qty;
        }
    }
}

void Remember(std::vector<RememberedBook>& memory, const transport::BusBookUpdate& update) {
    if (update.component_id.empty()) {
        return;
    }
    RememberedBook& book = FindBook(memory, update.component_id);
    const bool replace = update.kind != transport::BusBookPatchKind::Diff;
    ApplySide(book.has_bid, book.bid, book.bid_qty, update.bids, replace, true);
    ApplySide(book.has_ask, book.ask, book.ask_qty, update.asks, replace, false);
}

void RememberTick(const transport::BusTick& tick, std::vector<RememberedBook>& memory) {
    for (std::uint32_t i = 0; i < tick.n_books; ++i) {
        Remember(memory, tick.books[i]);
    }
    for (const auto& slot : tick.slots) {
        for (std::uint32_t i = 0; i < slot.n_books; ++i) {
            Remember(memory, slot.books[i]);
        }
    }
}

}  // namespace

void NoteTrades(const transport::BusTick& tick, DensePending& pending, RunStats& stats) {
    for (const auto& slot : tick.slots) {
        for (const auto& tr : slot.trades) {
            if (tr.exec_type == transport::BusExecType::Cancel) {
                ++stats.cancels;
                pending.Erase(tr.client_order_id);
                continue;
            }
            if (tr.exec_type != transport::BusExecType::Fill &&
                tr.exec_type != transport::BusExecType::Unspecified) {
                continue;
            }
            ++stats.fills;
            PendingOrder* po = pending.Find(tr.client_order_id);
            LOG_TRACE("fill pop_ts={} slot={} oid={} {} {} px={}+{} qty={} leaves={}", tick.pop_ts_ns,
                      slot.slot_id, tr.client_order_id,
                      po == nullptr ? "unknown" : (po->side == BusSide::Buy ? "buy" : "sell"),
                      tr.component_id, tr.price.whole, tr.price.frac, tr.qty, tr.leaves);
            if (po == nullptr) {
                continue;
            }
            const std::uint64_t delay = tick.pop_ts_ns >= po->submit_pop_ts_ns
                                            ? tick.pop_ts_ns - po->submit_pop_ts_ns
                                            : 0;
            stats.sum_fill_delay_ns += delay;
            if (delay > stats.max_fill_delay_ns) {
                stats.max_fill_delay_ns = delay;
            }
            if (delay < stats.min_fill_delay_ns) {
                stats.min_fill_delay_ns = delay;
            }
            if (tr.leaves == 0) {
                pending.Erase(tr.client_order_id);
            }
        }
    }
}

void MakeTakeBatch(const transport::BusTick& tick, int tick_n, int order_every,
                   std::uint64_t& next_oid, DensePending& pending, transport::BusOrderBatch& batch,
                   std::vector<RememberedBook>& memory) {
    RememberTick(tick, memory);
    batch.orders.clear();
    batch.cancels.clear();
    if (order_every < 1 || tick_n % order_every != 0 || memory.empty()) {
        return;
    }
    const bool buy_ask = ((tick_n / order_every) % 2) == 0;
    const std::uint32_t slot_id = tick.slots.empty() ? 1u : tick.slots[0].slot_id;

    auto push = [&](BusSide side, const RememberedBook& book, const transport::BusPrice& px) {
        const std::uint64_t oid = next_oid++;
        batch.orders.push_back(BusOrder{
            .slot_id = slot_id,
            .client_order_id = oid,
            .component_id = book.component_id,
            .side = side,
            .price = px,
            .qty = 1,
            .tif = BusTimeInForce::Ioc,
        });
        pending.Put(oid, PendingOrder{
                             .submit_pop_ts_ns = tick.pop_ts_ns,
                             .component_id = book.component_id,
                             .side = side,
                             .price = px,
                         });
    };

    auto price_ok = [](const transport::BusPrice& px) { return px.whole > 0 || px.frac > 0; };

    for (const RememberedBook& book : memory) {
        if (buy_ask) {
            if (book.has_ask && book.ask_qty > 0 && price_ok(book.ask)) {
                push(BusSide::Buy, book, book.ask);
            }
        } else if (book.has_bid && book.bid_qty > 0 && price_ok(book.bid)) {
            push(BusSide::Sell, book, book.bid);
        }
    }
}

}  // namespace client
