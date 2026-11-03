#include "population/population.hpp"

#include "population/book_diff.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace population {

Population::Population(std::string component_id, double tick_size, TapePtr tape,
                       std::vector<SlotConfig> slots)
    : component_id_(std::move(component_id))
    , tick_size_(tick_size)
    , slots_(std::move(slots))
    , book_(std::move(tape)) {
    if (component_id_.empty()) {
        throw std::invalid_argument("population: component_id must be non-empty");
    }
    if (!(tick_size_ > 0.0)) {
        throw std::invalid_argument("population: tick_size must be > 0");
    }
    ValidateSlots(slots_);
    for (const SlotConfig& cfg : slots_) {
        slot_send_.emplace(cfg.slot_id, SlotSendState{});
        slot_orders_.emplace(cfg.slot_id, SlotOrders{});
        slot_impact_.emplace(cfg.slot_id, ImpactState{cfg.impact_lambda});
        pending_trades_.emplace(cfg.slot_id, std::vector<Trade>{});
    }
}

bool Population::HasSlot(std::uint32_t slot_id) const {
    return slot_send_.find(slot_id) != slot_send_.end();
}

const SlotConfig* Population::FindSlot(std::uint32_t slot_id) const {
    for (const SlotConfig& cfg : slots_) {
        if (cfg.slot_id == slot_id) {
            return &cfg;
        }
    }
    return nullptr;
}

const SlotOrders* Population::SlotBook(std::uint32_t slot_id) const {
    const auto it = slot_orders_.find(slot_id);
    if (it == slot_orders_.end()) {
        return nullptr;
    }
    return &it->second;
}

const ImpactState* Population::SlotImpact(std::uint32_t slot_id) const {
    const auto it = slot_impact_.find(slot_id);
    if (it == slot_impact_.end()) {
        return nullptr;
    }
    return &it->second;
}

long long Population::ToTicks(double price) const {
    return static_cast<long long>(std::llround(price / tick_size_));
}

bool Population::OnGrid(double price) const {
    if (!(price > 0.0) || !std::isfinite(price)) {
        return false;
    }
    const double on_grid = static_cast<double>(ToTicks(price)) * tick_size_;
    return std::abs(price - on_grid) < tick_size_ / 1024.0;
}

void Population::Start() {
    if (started_) {
        throw std::logic_error("population: already started");
    }
    book_.Start(clock_);
    started_ = true;
}

void Population::QueueResync(std::uint32_t slot_id) {
    if (!HasSlot(slot_id)) {
        throw std::invalid_argument("population: QueueResync unknown slot_id " +
                                    std::to_string(slot_id));
    }
    resync_slots_.insert(slot_id);
}

SnapshotEvent Population::ViewSnap(const SlotConfig& cfg) const {
    const std::uint64_t pop = clock_.pop_ts_ns();
    const std::uint64_t view_ts =
        (cfg.md_lag_ns >= pop) ? 0 : (pop - cfg.md_lag_ns);

    if (auto snap = book_.window().AtOrBefore(view_ts)) {
        return *snap;
    }
    // Warm-up: lag reaches before SnapshotFloor → clamp to oldest kept snap.
    if (book_.window().empty()) {
        throw std::logic_error("population: ViewSnap on empty TapeWindow");
    }
    auto floor = book_.window().AtOrBefore(book_.window().floor_ts_ns());
    if (!floor) {
        throw std::logic_error("population: TapeWindow floor missing");
    }
    return *floor;
}

SnapshotEvent Population::EffectiveView(const SlotConfig& cfg) const {
    SnapshotEvent view = ViewSnap(cfg);
    slot_impact_.at(cfg.slot_id).ApplyTo(view.bids, view.asks, tick_size_);
    return view;
}

void Population::TrimWindowForLags() {
    if (book_.window().empty() || slots_.empty()) {
        return;
    }
    std::uint64_t max_lag = 0;
    for (const SlotConfig& cfg : slots_) {
        max_lag = std::max(max_lag, cfg.md_lag_ns);
    }
    const std::uint64_t pop = clock_.pop_ts_ns();
    // Warm-up (max_lag >= pop → view_ts under floor): nothing to drop yet.
    if (max_lag >= pop) {
        return;
    }
    if (auto cover = book_.window().AtOrBefore(pop - max_lag)) {
        book_.window().TrimBefore(cover->ts_ns);
    }
}

void Population::MatchDueIfNeeded() {
    const std::uint64_t pop = clock_.pop_ts_ns();
    if (matched_this_pop_ && last_match_pop_ts_ns_ == pop) {
        return;
    }

    for (const SlotConfig& cfg : slots_) {
        std::vector<Level> bids = book_.bids();
        std::vector<Level> asks = book_.asks();
        ImpactState& impact = slot_impact_.at(cfg.slot_id);
        impact.Recover(book_.bids(), book_.asks(), pop, tick_size_);
        impact.ApplyTo(bids, asks, tick_size_);
        pending_trades_[cfg.slot_id] =
            slot_orders_.at(cfg.slot_id).MatchDue(pop, std::move(bids), std::move(asks),
                                                  tick_size_, &impact);
    }

    last_match_pop_ts_ns_ = pop;
    matched_this_pop_ = true;
}

TickView Population::MakeTick() {
    if (!started_) {
        throw std::logic_error("population: MakeTick before Start");
    }

    MatchDueIfNeeded();

    TickView tick;
    tick.pop_ts_ns = clock_.pop_ts_ns();
    tick.slots.reserve(slots_.size());
    // Shared tick.books unused when md_lag may differ per slot.

    for (const SlotConfig& cfg : slots_) {
        SlotTickView slot_tick;
        slot_tick.slot_id = cfg.slot_id;
        slot_tick.trades = std::move(pending_trades_[cfg.slot_id]);
        pending_trades_[cfg.slot_id].clear();

        const SnapshotEvent view = EffectiveView(cfg);
        SlotSendState& sent = slot_send_.at(cfg.slot_id);
        const bool slot_resync = resync_slots_.erase(cfg.slot_id) > 0;
        const bool need_keyframe = !sent.has_sent || slot_resync;

        if (need_keyframe) {
            SlotBookUpdateView patch;
            patch.component_id = component_id_;
            patch.kind = BookPatchKind::Keyframe;
            patch.bids = view.bids;
            patch.asks = view.asks;
            slot_tick.books.push_back(std::move(patch));
            sent.last_bids = view.bids;
            sent.last_asks = view.asks;
            sent.has_sent = true;
        } else if (const BookDiffResult diff =
                       DiffBooks(sent.last_bids, sent.last_asks, view.bids, view.asks, tick_size_);
                   !diff.empty()) {
            SlotBookUpdateView patch;
            patch.component_id = component_id_;
            patch.kind = BookPatchKind::Diff;
            patch.bids = diff.bids;
            patch.asks = diff.asks;
            slot_tick.books.push_back(std::move(patch));

            sent.last_bids = view.bids;
            sent.last_asks = view.asks;
        }

        tick.slots.push_back(std::move(slot_tick));
    }

    TrimWindowForLags();
    return tick;
}

bool Population::Advance() {
    if (!started_) {
        throw std::logic_error("population: Advance before Start");
    }
    const bool ok = book_.Advance(clock_);
    if (ok) {
        matched_this_pop_ = false;
    }
    return ok;
}

OrderResult Population::ValidateNewOrder(const NewOrder& order) const {
    if (!started_) {
        return OrderResult::NotStarted;
    }
    if (!HasSlot(order.slot_id)) {
        return OrderResult::UnknownSlot;
    }
    if (order.component_id != component_id_) {
        return OrderResult::UnknownComponent;
    }
    if (order.qty == 0) {
        return OrderResult::InvalidQty;
    }
    if (!OnGrid(order.price)) {
        return OrderResult::PriceOffGrid;
    }
    if (order.side == Side::Unknown) {
        return OrderResult::InvalidSide;
    }
    if (order.tif == TimeInForce::Unknown) {
        return OrderResult::InvalidTif;
    }
    if (order.client_order_id == 0) {
        return OrderResult::InvalidClientOrderId;
    }
    return OrderResult::Ok;
}

OrderResult Population::SubmitOne(const NewOrder& order) {
    const OrderResult check = ValidateNewOrder(order);
    if (check != OrderResult::Ok) {
        return check;
    }

    const SlotConfig* cfg = FindSlot(order.slot_id);
    WorkingOrder working;
    working.client_order_id = order.client_order_id;
    working.component_id = order.component_id;
    working.side = order.side;
    working.price_ticks = ToTicks(order.price);
    working.leaves = order.qty;
    working.tif = order.tif;
    working.effective_ts_ns = clock_.pop_ts_ns() + cfg->delta_ns;

    return slot_orders_.at(order.slot_id).Accept(std::move(working));
}

OrderResult Population::CancelOne(const CancelOrder& cancel) {
    if (!started_) {
        return OrderResult::NotStarted;
    }
    if (!HasSlot(cancel.slot_id)) {
        return OrderResult::UnknownSlot;
    }
    return slot_orders_.at(cancel.slot_id).Cancel(cancel.client_order_id);
}

OrderResult Population::ValidateCancel(const CancelOrder& cancel) const {
    if (!started_) {
        return OrderResult::NotStarted;
    }
    if (!HasSlot(cancel.slot_id)) {
        return OrderResult::UnknownSlot;
    }
    if (cancel.client_order_id == 0) {
        return OrderResult::InvalidClientOrderId;
    }
    if (slot_orders_.at(cancel.slot_id).FindLive(cancel.client_order_id) == nullptr) {
        return OrderResult::NoSuchOrder;
    }
    return OrderResult::Ok;
}

OrderBatchResult Population::Submit(const OrderBatch& batch) {
    OrderBatchResult result;
    result.order_results.reserve(batch.orders.size());
    result.cancel_results.reserve(batch.cancels.size());

    // All-or-nothing: validate the whole batch first (wire → OrdersAck vs Error).
    // Walk every item for per-line codes; all_ok avoids a second scan.
    bool all_ok = true;
    std::unordered_set<OrderKey, OrderKeyHash> pending_new;
    for (const NewOrder& order : batch.orders) {
        OrderResult check = ValidateNewOrder(order);
        if (check == OrderResult::Ok) {
            const OrderKey key{order.slot_id, order.client_order_id};
            const bool dup_live =
                slot_orders_.at(order.slot_id).FindLive(order.client_order_id) != nullptr;
            const bool dup_batch = pending_new.count(key) > 0;
            if (dup_live || dup_batch) {
                check = OrderResult::DuplicateOrderId;
            } else {
                pending_new.insert(key);
            }
        }
        if (check != OrderResult::Ok) {
            all_ok = false;
        }
        result.order_results.push_back(check);
    }

    std::unordered_set<OrderKey, OrderKeyHash> pending_cancel;
    for (const CancelOrder& cancel : batch.cancels) {
        OrderResult check = ValidateCancel(cancel);
        const OrderKey key{cancel.slot_id, cancel.client_order_id};
        if (check == OrderResult::NoSuchOrder && pending_new.count(key) > 0) {
            // Cancel targets an order introduced earlier in this same batch.
            check = OrderResult::Ok;
        }
        if (check == OrderResult::Ok) {
            if (pending_cancel.count(key) > 0) {
                check = OrderResult::NoSuchOrder;
            } else {
                pending_cancel.insert(key);
            }
        }
        if (check != OrderResult::Ok) {
            all_ok = false;
        }
        result.cancel_results.push_back(check);
    }

    if (!all_ok) {
        return result;
    }

    for (const NewOrder& order : batch.orders) {
        (void)SubmitOne(order);
    }
    for (const CancelOrder& cancel : batch.cancels) {
        (void)CancelOne(cancel);
    }
    return result;
}

}  // namespace population
