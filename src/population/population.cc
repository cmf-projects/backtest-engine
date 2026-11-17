#include "population/population.hpp"

#include "population/book_diff.hpp"
#include "population/price_ticks.hpp"
#include "population/snapshot_assign.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace population {

namespace {

constexpr std::size_t kMaxComponents = 3;

std::vector<ComponentSpec> MakeSingleSpec(std::string component_id, double tick_size,
                                          TapePtr tape) {
    std::vector<ComponentSpec> specs;
    specs.push_back(ComponentSpec{.component_id = std::move(component_id),
                                  .tick_size = tick_size,
                                  .tape = std::move(tape)});
    return specs;
}

// Highest id among earlier new-orders of this slot in the batch, and the last id
// this slot has actually applied. The candidate must be strictly above both.
[[nodiscard]] std::uint64_t NewOrderFloor(const OrderBatch& batch, std::size_t index,
                                          std::uint32_t slot_id, std::uint64_t last_accepted) {
    std::uint64_t floor = last_accepted;
    for (std::size_t i = 0; i < index; ++i) {
        const NewOrder& prior = batch.orders[i];
        if (prior.slot_id == slot_id && prior.client_order_id > floor) {
            floor = prior.client_order_id;
        }
    }
    return floor;
}

}  // namespace

const SnapshotEvent& Component::current() const {
    if (!current_) {
        throw std::logic_error("population: Component has no current snapshot");
    }
    return *current_;
}

Population::Population(std::vector<ComponentSpec> components, std::vector<SlotConfig> slots)
    : slots_(std::move(slots)) {
    if (components.empty() || components.size() > kMaxComponents) {
        throw std::invalid_argument("population: components must be 1…3");
    }

    std::vector<MergedComponent> merged;
    merged.reserve(components.size());
    components_.reserve(components.size());
    for (ComponentSpec& spec : components) {
        if (spec.component_id.empty()) {
            throw std::invalid_argument("population: component_id must be non-empty");
        }
        if (!(spec.tick_size > 0.0)) {
            throw std::invalid_argument("population: tick_size must be > 0");
        }
        if (!spec.tape) {
            throw std::invalid_argument("population: null tape");
        }
        Component state;
        state.component_id_ = spec.component_id;
        state.tick_size_ = spec.tick_size;
        components_.push_back(std::move(state));
        merged.push_back(MergedComponent{.component_id = spec.component_id,
                                         .tape = std::move(spec.tape)});
    }
    merge_.emplace(std::move(merged));

    match_bids_scratch_.reserve(kMaxLevelsPerSide);
    match_asks_scratch_.reserve(kMaxLevelsPerSide);
    view_scratch_.bids.reserve(kMaxLevelsPerSide);
    view_scratch_.asks.reserve(kMaxLevelsPerSide);
    diff_scratch_.bids.reserve(kMaxDiffLevelsPerSide);
    diff_scratch_.asks.reserve(kMaxDiffLevelsPerSide);

    ValidateSlots(slots_);
    for (const SlotConfig& cfg : slots_) {
        slot_send_.emplace(cfg.slot_id, SlotSendState{});
        slot_orders_.emplace(cfg.slot_id, SlotOrders{});
        std::vector<Trade> trades;
        trades.reserve(kTradeCapacity);
        pending_trades_.emplace(cfg.slot_id, std::move(trades));
        for (const Component& component : components_) {
            ComponentSend& send = slot_send_.at(cfg.slot_id).components[component.component_id()];
            send.last_bids.reserve(kMaxLevelsPerSide);
            send.last_asks.reserve(kMaxLevelsPerSide);
            slot_impact_.emplace(SlotComponentKey{cfg.slot_id, component.component_id()},
                                 ImpactState{cfg.impact_lambda});
        }
    }
}

// Single-component convenience (existing tests / P0 call sites).
Population::Population(std::string component_id, double tick_size, TapePtr tape,
                       std::vector<SlotConfig> slots)
    : Population(MakeSingleSpec(std::move(component_id), tick_size, std::move(tape)),
                 std::move(slots)) {}

bool Population::HasSlot(std::uint32_t slot_id) const {
    return slot_send_.find(slot_id) != slot_send_.end();
}

bool Population::HasComponent(const std::string& component_id) const {
    return FindComponent(component_id) != nullptr;
}

Component* Population::MutableComponent(const std::string& component_id) {
    for (Component& component : components_) {
        if (component.component_id_ == component_id) {
            return &component;
        }
    }
    return nullptr;
}

const SlotConfig* Population::FindSlot(std::uint32_t slot_id) const {
    for (const SlotConfig& cfg : slots_) {
        if (cfg.slot_id == slot_id) {
            return &cfg;
        }
    }
    return nullptr;
}

const Component* Population::FindComponent(const std::string& component_id) const {
    for (const Component& component : components_) {
        if (component.component_id_ == component_id) {
            return &component;
        }
    }
    return nullptr;
}

const std::string& Population::component_id() const {
    if (components_.size() != 1) {
        throw std::logic_error("population: component_id() requires exactly one component");
    }
    return components_.front().component_id_;
}

const Component& Population::book() const {
    if (components_.size() != 1) {
        throw std::logic_error("population: book() requires exactly one component");
    }
    return components_.front();
}

const SlotOrders* Population::SlotBook(std::uint32_t slot_id) const {
    const auto it = slot_orders_.find(slot_id);
    if (it == slot_orders_.end()) {
        return nullptr;
    }
    return &it->second;
}

const ImpactState* Population::SlotImpact(std::uint32_t slot_id,
                                          const std::string& component_id) const {
    const auto it = slot_impact_.find(SlotComponentKey{slot_id, component_id});
    if (it == slot_impact_.end()) {
        return nullptr;
    }
    return &it->second;
}

const ImpactState* Population::SlotImpact(std::uint32_t slot_id) const {
    if (components_.size() != 1) {
        throw std::logic_error("population: SlotImpact(slot) requires exactly one component");
    }
    return SlotImpact(slot_id, components_.front().component_id_);
}

bool Population::OnGrid(double price, double tick_size) const {
    if (!(price > 0.0) || !std::isfinite(price)) {
        return false;
    }
    const double on_grid = static_cast<double>(PriceToTicks(price, tick_size)) * tick_size;
    return std::abs(price - on_grid) < tick_size / 1024.0;
}

void Population::ApplyMergedStep(const MergedStep& step) {
    for (const ComponentSnap& snap : step.components) {
        // Keep-last rows: current_ already holds the file snap — skip copy.
        if (!snap.updated) {
            continue;
        }
        Component* component = MutableComponent(snap.component_id);
        if (component == nullptr) {
            throw std::logic_error("population: unknown component in MergedStep: " +
                                   snap.component_id);
        }
        if (snap.snap == nullptr) {
            throw std::logic_error("population: null ComponentSnap::snap");
        }
        if (!component->current_.has_value()) {
            component->current_.emplace();
            component->current_->bids.reserve(kMaxLevelsPerSide);
            component->current_->asks.reserve(kMaxLevelsPerSide);
        }
        AssignSnapshot(*component->current_, *snap.snap);
        component->window_.Append(*component->current_);
    }
}

void Population::Start() {
    if (started_) {
        throw std::logic_error("population: already started");
    }
    if (!merge_->NextInto(step_scratch_)) {
        throw std::runtime_error("population: empty merged tape on Start");
    }
    clock_.Start(step_scratch_.ts_ns);
    ApplyMergedStep(step_scratch_);
    started_ = true;
}

const SnapshotEvent* Population::ViewSnap(const SlotConfig& cfg,
                                          const Component& component) const {
    const std::uint64_t pop = clock_.pop_ts_ns();
    // view_ts would be before the dataset. A snap at 0 is not visible yet.
    if (cfg.md_lag_ns > pop || component.window().empty()) {
        return nullptr;
    }
    return component.window().AtOrBefore(pop - cfg.md_lag_ns);
}

bool Population::EffectiveViewInto(const SlotConfig& cfg, const Component& component,
                                   SnapshotEvent& out) const {
    // TZ §8.4 P0: old file snap @ view_ts, minus *current* eaten (not historical).
    const SnapshotEvent* snap = ViewSnap(cfg, component);
    if (snap == nullptr) {
        return false;
    }
    AssignSnapshot(out, *snap);
    slot_impact_.at(SlotComponentKey{cfg.slot_id, component.component_id()})
        .ApplyTo(out.bids, out.asks, component.tick_size());
    return true;
}

void Population::TrimWindowForLags() {
    if (slots_.empty()) {
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
    const std::uint64_t cover_ts = pop - max_lag;
    for (Component& component : components_) {
        if (component.window_.empty()) {
            continue;
        }
        if (const SnapshotEvent* cover = component.window_.AtOrBefore(cover_ts)) {
            component.window_.TrimBefore(cover->ts_ns);
        }
    }
}

void Population::MatchDueIfNeeded() {
    const std::uint64_t pop = clock_.pop_ts_ns();
    if (matched_this_pop_ && last_match_pop_ts_ns_ == pop) {
        return;
    }

    for (const SlotConfig& cfg : slots_) {
        auto& pending = pending_trades_[cfg.slot_id];
        pending.clear();
        for (const Component& component : components_) {
            if (!component.present()) {
                continue;
            }
            match_bids_scratch_ = component.current().bids;
            match_asks_scratch_ = component.current().asks;
            ImpactState& impact =
                slot_impact_.at(SlotComponentKey{cfg.slot_id, component.component_id()});
            impact.Recover(component.current().bids, component.current().asks, pop,
                           component.tick_size());
            impact.ApplyTo(match_bids_scratch_, match_asks_scratch_, component.tick_size());
            slot_orders_.at(cfg.slot_id)
                .MatchDue(pop, match_bids_scratch_, match_asks_scratch_, component.tick_size(),
                          &impact, component.component_id(), pending);
        }
    }

    last_match_pop_ts_ns_ = pop;
    matched_this_pop_ = true;
}

namespace {

[[nodiscard]] bool SameLevels(const std::vector<Level>& a, const std::vector<Level>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].price != b[i].price || a[i].qty != b[i].qty) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool Population::SeedSharedFirstTick() {
    for (const auto& [_, state] : slot_send_) {
        for (const auto& [__, send] : state.components) {
            if (send.has_sent) {
                return false;
            }
        }
    }
    if (slots_.empty()) {
        return false;
    }

    struct Sides {
        std::vector<Level> bids;
        std::vector<Level> asks;
    };
    auto capture = [&](const SlotConfig& cfg, const Component& component, Sides& out) -> bool {
        const ImpactState& impact =
            slot_impact_.at(SlotComponentKey{cfg.slot_id, component.component_id()});
        if (!impact.enabled()) {
            const SnapshotEvent* snap = ViewSnap(cfg, component);
            if (snap == nullptr) {
                return false;
            }
            out.bids = snap->bids;
            out.asks = snap->asks;
            return true;
        }
        if (!EffectiveViewInto(cfg, component, view_scratch_)) {
            return false;
        }
        out.bids = view_scratch_.bids;
        out.asks = view_scratch_.asks;
        return true;
    };

    std::vector<SharedBookView> shared;
    for (const Component& component : components_) {
        if (!component.present()) {
            continue;
        }
        Sides seed;
        if (!capture(slots_.front(), component, seed)) {
            return false;
        }
        for (std::size_t si = 1; si < slots_.size(); ++si) {
            Sides other;
            if (!capture(slots_[si], component, other) ||
                !SameLevels(seed.bids, other.bids) || !SameLevels(seed.asks, other.asks)) {
                return false;
            }
        }
        SharedBookView book;
        book.component_id = component.component_id();
        book.kind = BookPatchKind::Keyframe;
        book.bids = std::move(seed.bids);
        book.asks = std::move(seed.asks);
        shared.push_back(std::move(book));
    }

    for (const SharedBookView& book : shared) {
        for (const SlotConfig& cfg : slots_) {
            ComponentSend& send = slot_send_.at(cfg.slot_id).components[book.component_id];
            send.last_bids = book.bids;
            send.last_asks = book.asks;
            send.has_sent = true;
        }
    }
    tick_scratch_.books = std::move(shared);
    return true;
}

const TickView& Population::MakeTick() {
    if (!started_) {
        throw std::logic_error("population: MakeTick before Start");
    }

    MatchDueIfNeeded();

    tick_scratch_.pop_ts_ns = clock_.pop_ts_ns();
    tick_scratch_.books.clear();
    if (tick_scratch_.slots.size() != slots_.size()) {
        const std::size_t born = tick_scratch_.slots.size();
        tick_scratch_.slots.resize(slots_.size());
        for (std::size_t i = born; i < tick_scratch_.slots.size(); ++i) {
            tick_scratch_.slots[i].trades.reserve(kTradeCapacity);
        }
    }
    // Shared keyframe once, when every slot still sees the same snap.
    const bool shared = SeedSharedFirstTick();

    for (std::size_t si = 0; si < slots_.size(); ++si) {
        const SlotConfig& cfg = slots_[si];
        SlotTickView& slot_tick = tick_scratch_.slots[si];
        slot_tick.slot_id = cfg.slot_id;
        // Swap keeps a capacitous trades buffer in pending_trades_ for MatchDue.
        slot_tick.trades.swap(pending_trades_[cfg.slot_id]);
        pending_trades_[cfg.slot_id].clear();
        if (shared) {
            slot_tick.n_books = 0;
            continue;
        }

        SlotSendState& sent = slot_send_.at(cfg.slot_id);
        std::size_t book_n = 0;
        auto take_patch = [&]() -> SlotBookUpdateView& {
            if (book_n >= slot_tick.books.size()) {
                slot_tick.books.resize(book_n + 1);
                // Born here: a sent side is at most one book. Later ticks reuse it.
                slot_tick.books.back().bids.reserve(kMaxDiffLevelsPerSide);
                slot_tick.books.back().asks.reserve(kMaxDiffLevelsPerSide);
            }
            return slot_tick.books[book_n++];
        };
        for (const Component& component : components_) {
            if (!component.present()) {
                continue;
            }
            const ImpactState& impact =
                slot_impact_.at(SlotComponentKey{cfg.slot_id, component.component_id()});
            const std::vector<Level>* view_bids = nullptr;
            const std::vector<Level>* view_asks = nullptr;
            if (!impact.enabled()) {
                const SnapshotEvent* file_view = ViewSnap(cfg, component);
                if (file_view == nullptr) {
                    continue;
                }
                view_bids = &file_view->bids;
                view_asks = &file_view->asks;
            } else {
                if (!EffectiveViewInto(cfg, component, view_scratch_)) {
                    continue;
                }
                view_bids = &view_scratch_.bids;
                view_asks = &view_scratch_.asks;
            }
            ComponentSend& send = sent.components[component.component_id()];

            if (!send.has_sent) {
                SlotBookUpdateView& patch = take_patch();
                patch.component_id = component.component_id();
                patch.kind = BookPatchKind::Keyframe;
                patch.bids = *view_bids;
                patch.asks = *view_asks;
                send.last_bids = *view_bids;
                send.last_asks = *view_asks;
                send.has_sent = true;
            } else {
                DiffBooksInto(send.last_bids, send.last_asks, *view_bids, *view_asks,
                              component.tick_size(), diff_scratch_);
                if (!diff_scratch_.empty()) {
                    SlotBookUpdateView& patch = take_patch();
                    patch.component_id = component.component_id();
                    // One kind for both sides. A side longer than the book limit
                    // is replaced by the full view, not a 128-row diff.
                    if (diff_scratch_.bids.size() > kMaxLevelsPerSide ||
                        diff_scratch_.asks.size() > kMaxLevelsPerSide) {
                        patch.kind = BookPatchKind::Keyframe;
                        patch.bids = *view_bids;
                        patch.asks = *view_asks;
                    } else {
                        patch.kind = BookPatchKind::Diff;
                        patch.bids = diff_scratch_.bids;
                        patch.asks = diff_scratch_.asks;
                    }
                    send.last_bids = *view_bids;
                    send.last_asks = *view_asks;
                }
            }
        }
        slot_tick.n_books = static_cast<std::uint32_t>(book_n);
        for (std::size_t i = book_n; i < slot_tick.books.size(); ++i) {
            SlotBookUpdateView& stale = slot_tick.books[i];
            stale.component_id.clear();
            stale.bids.clear();
            stale.asks.clear();
        }
    }

    TrimWindowForLags();
    return tick_scratch_;
}

bool Population::Advance() {
    if (!started_) {
        throw std::logic_error("population: Advance before Start");
    }
    if (ended_) {
        return false;
    }
    if (step_ns_ == -1) {
        if (!merge_->NextInto(step_scratch_)) {
            ended_ = true;
            return false;
        }
        clock_.AdvanceTo(step_scratch_.ts_ns);
        ApplyMergedStep(step_scratch_);
        matched_this_pop_ = false;
        return true;
    }

    const auto pending_ts = merge_->PeekTs();
    if (!pending_ts.has_value()) {
        ended_ = true;
        return false;
    }
    const std::uint64_t pop = clock_.pop_ts_ns();
    std::uint64_t next = 0;
    if (*pending_ts > pop && (*pending_ts - pop) > kSessionGapNs) {
        next = *pending_ts;
    } else {
        if (static_cast<std::uint64_t>(step_ns_) > std::numeric_limits<std::uint64_t>::max() - pop) {
            ended_ = true;
            return false;
        }
        next = pop + static_cast<std::uint64_t>(step_ns_);
    }
    while (const auto pending = merge_->PeekTs()) {
        if (*pending > next) {
            break;
        }
        if (!merge_->NextInto(step_scratch_)) {
            ended_ = true;
            return false;
        }
        ApplyMergedStep(step_scratch_);
    }
    clock_.AdvanceTo(next);
    matched_this_pop_ = false;
    return true;
}

void Population::SetStepNs(std::int64_t step_ns) {
    if (step_ns == 0 || step_ns < -1) {
        throw std::invalid_argument("population: step_ns must be -1 or > 0");
    }
    step_ns_ = step_ns;
}

OrderResult Population::ValidateNewOrder(const NewOrder& order) const {
    if (!started_) {
        return OrderResult::NotStarted;
    }
    if (!HasSlot(order.slot_id)) {
        return OrderResult::UnknownSlot;
    }
    const Component* component = FindComponent(order.component_id);
    if (component == nullptr || !component->present()) {
        return OrderResult::UnknownComponent;
    }
    if (order.qty == 0) {
        return OrderResult::InvalidQty;
    }
    if (!OnGrid(order.price, component->tick_size())) {
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
    const Component* component = FindComponent(order.component_id);
    WorkingOrder working;
    working.client_order_id = order.client_order_id;
    working.component_id = order.component_id;
    working.side = order.side;
    working.price_ticks = PriceToTicks(order.price, component->tick_size());
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
    const SlotConfig* cfg = FindSlot(cancel.slot_id);
    return slot_orders_.at(cancel.slot_id)
        .ScheduleCancel(cancel.client_order_id, clock_.pop_ts_ns() + cfg->delta_ns);
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

const OrderBatchResult& Population::Submit(const OrderBatch& batch) {
    auto& result = submit_result_;
    result.order_results.clear();
    result.cancel_results.clear();
    if (result.order_results.capacity() < batch.orders.size()) {
        result.order_results.reserve(batch.orders.size());
    }
    if (result.cancel_results.capacity() < batch.cancels.size()) {
        result.cancel_results.reserve(batch.cancels.size());
    }

    // All-or-nothing. Cancels see only orders already live from earlier ticks.
    // An id introduced in this same batch is not live yet, so its cancel is an error.
    // A new id must be strictly above the last applied id of its slot and above
    // earlier new ids of that slot in this batch. The watermark moves only if
    // the whole batch applies, so a rejected batch can be resent.
    bool all_ok = true;
    if (!batch.cancels.empty()) {
        if (++cancel_stamp_ == 0) {
            for (auto& entry : slot_orders_) {
                entry.second.ClearCancelStamps();
            }
            cancel_stamp_ = 1;
        }
    }
    for (const CancelOrder& cancel : batch.cancels) {
        OrderResult check = ValidateCancel(cancel);
        if (check == OrderResult::Ok) {
            check = slot_orders_.at(cancel.slot_id)
                        .NoteCancel(cancel.client_order_id, cancel_stamp_);
        }
        if (check != OrderResult::Ok) {
            all_ok = false;
        }
        result.cancel_results.push_back(check);
    }

    for (std::size_t i = 0; i < batch.orders.size(); ++i) {
        const NewOrder& order = batch.orders[i];
        OrderResult check = ValidateNewOrder(order);
        if (check == OrderResult::Ok) {
            const std::uint64_t floor = NewOrderFloor(
                batch, i, order.slot_id, slot_orders_.at(order.slot_id).last_accepted_oid());
            if (order.client_order_id <= floor) {
                check = OrderResult::DuplicateOrderId;
            }
        }
        if (check != OrderResult::Ok) {
            all_ok = false;
        }
        result.order_results.push_back(check);
    }

    if (!all_ok) {
        return result;
    }

    for (const CancelOrder& cancel : batch.cancels) {
        (void)CancelOne(cancel);
    }
    for (const NewOrder& order : batch.orders) {
        (void)SubmitOne(order);
        slot_orders_.at(order.slot_id).NoteAcceptedOid(order.client_order_id);
    }
    return result;
}

}  // namespace population
