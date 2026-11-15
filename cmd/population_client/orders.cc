#include "population_venue/log/log.hpp"
#include "orders.hpp"

using transport::BusOrder;
using transport::BusSide;
using transport::BusTimeInForce;

namespace client {
namespace {

void CollectBooks(const transport::BusTick& tick, std::vector<const transport::BusBookUpdate*>& out) {
    out.clear();
    if (!tick.slots.empty()) {
        for (std::uint32_t i = 0; i < tick.slots[0].n_books; ++i) {
            const auto& book = tick.slots[0].books[i];
            if (book.bids.empty() && book.asks.empty()) {
                continue;
            }
            out.push_back(&book);
        }
    }
    if (out.empty()) {
        for (std::uint32_t i = 0; i < tick.n_books; ++i) {
            const auto& book = tick.books[i];
            if (book.bids.empty() && book.asks.empty()) {
                continue;
            }
            out.push_back(&book);
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

void MakeTakeBatch(const transport::BusTick& tick, std::uint64_t& next_oid, DensePending& pending,
                   transport::BusOrderBatch& batch,
                   std::vector<const transport::BusBookUpdate*>& books) {
    batch.orders.clear();
    batch.cancels.clear();
    CollectBooks(tick, books);
    if (books.empty()) {
        return;
    }
    const std::uint32_t slot_id = tick.slots.empty() ? 1u : tick.slots[0].slot_id;

    auto push = [&](BusSide side, const transport::BusBookUpdate& book, const transport::BusPrice& px) {
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

    for (const transport::BusBookUpdate* book : books) {
        if (book == nullptr) {
            continue;
        }
        if (!book->asks.empty() && book->asks[0].qty > 0 && price_ok(book->asks[0].price)) {
            push(BusSide::Buy, *book, book->asks[0].price);
        }
        if (!book->bids.empty() && book->bids[0].qty > 0 && price_ok(book->bids[0].price)) {
            push(BusSide::Sell, *book, book->bids[0].price);
        }
    }
}

}  // namespace client
