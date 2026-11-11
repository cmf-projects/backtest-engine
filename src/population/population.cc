#include "population/population.hpp"

#include "population/book_diff.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
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

    ValidateSlots(slots_);
    for (const SlotConfig& cfg : slots_) {
        slot_send_.emplace(cfg.slot_id, SlotSendState{});
        slot_orders_.emplace(cfg.slot_id, SlotOrders{});
        pending_trades_.emplace(cfg.slot_id, std::vector<Trade>{});
        for (const Component& component : components_) {
            slot_impact_.emplace(SlotComponentKey{cfg.slot_id, component.component_id()},
                                 ImpactState{cfg.impact_lambda});
        }
    }
}

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

long long Population::ToTicks(double price, double tick_size) const {
    return static_cast<long long>(std::llround(price / tick_size));
}

bool Population::OnGrid(double price, double tick_size) const {
    if (!(price > 0.0) || !std::isfinite(price)) {
        return false;
    }
    const double on_grid = static_cast<double>(ToTicks(price, tick_size)) * tick_size;
    return std::abs(price - on_grid) < tick_size / 1024.0;
}

void Population::ApplyMergedStep(const MergedStep& step) {
    for (const ComponentSnap& snap : step.components) {
        Component* component = MutableComponent(snap.component_id);
        if (component == nullptr) {
            throw std::logic_error("population: unknown component in MergedStep: " +
                                   snap.component_id);
        }
        component->current_ = snap.snap;
        // Append only on real tape events (keep-last does not grow history).
        if (snap.updated) {
            component->window_.Append(snap.snap);
        }
    }
}

void Population::Start() {
    if (started_) {
        throw std::logic_error("population: already started");
    }
    auto step = merge_->Next();
    if (!step) {
        throw std::runtime_error("population: empty merged tape on Start");
    }
    clock_.Start(step->ts_ns);
    ApplyMergedStep(*step);
    started_ = true;
}

SnapshotEvent Population::ViewSnap(const SlotConfig& cfg, const Component& component) const {
    const std::uint64_t pop = clock_.pop_ts_ns();
    const std::uint64_t view_ts =
        (cfg.md_lag_ns >= pop) ? 0 : (pop - cfg.md_lag_ns);

    if (auto snap = component.window().AtOrBefore(view_ts)) {
        return *snap;
    }
    // Warm-up: lag reaches before SnapshotFloor → clamp to oldest kept snap.
    if (component.window().empty()) {
        throw std::logic_error("population: ViewSnap on empty TapeWindow");
    }
    auto floor = component.window().AtOrBefore(component.window().floor_ts_ns());
    if (!floor) {
        throw std::logic_error("population: TapeWindow floor missing");
    }
    return *floor;
}

SnapshotEvent Population::EffectiveView(const SlotConfig& cfg,
                                        const Component& component) const {
    SnapshotEvent view = ViewSnap(cfg, component);
    slot_impact_.at(SlotComponentKey{cfg.slot_id, component.component_id()})
        .ApplyTo(view.bids, view.asks, component.tick_size());
    return view;
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
        if (auto cover = component.window_.AtOrBefore(cover_ts)) {
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
        std::vector<Trade> all_trades;
        for (const Component& component : components_) {
            if (!component.present()) {
                continue;
            }
            std::vector<Level> bids = component.current().bids;
            std::vector<Level> asks = component.current().asks;
            ImpactState& impact =
                slot_impact_.at(SlotComponentKey{cfg.slot_id, component.component_id()});
            impact.Recover(component.current().bids, component.current().asks, pop,
                           component.tick_size());
            impact.ApplyTo(bids, asks, component.tick_size());
            auto trades =
                slot_orders_.at(cfg.slot_id)
                    .MatchDue(pop, std::move(bids), std::move(asks), component.tick_size(),
                              &impact, component.component_id());
            all_trades.insert(all_trades.end(), std::make_move_iterator(trades.begin()),
                              std::make_move_iterator(trades.end()));
        }
        pending_trades_[cfg.slot_id] = std::move(all_trades);
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

        SlotSendState& sent = slot_send_.at(cfg.slot_id);
        for (const Component& component : components_) {
            if (!component.present()) {
                continue;
            }
            const SnapshotEvent view = EffectiveView(cfg, component);
            ComponentSend& send = sent.components[component.component_id()];

            if (!send.has_sent) {
                SlotBookUpdateView patch;
                patch.component_id = component.component_id();
                patch.kind = BookPatchKind::Keyframe;
                patch.bids = view.bids;
                patch.asks = view.asks;
                slot_tick.books.push_back(std::move(patch));
                send.last_bids = view.bids;
                send.last_asks = view.asks;
                send.has_sent = true;
            } else if (const BookDiffResult diff =
                           DiffBooks(send.last_bids, send.last_asks, view.bids, view.asks,
                                     component.tick_size());
                       !diff.empty()) {
                SlotBookUpdateView patch;
                patch.component_id = component.component_id();
                patch.kind = BookPatchKind::Diff;
                patch.bids = diff.bids;
                patch.asks = diff.asks;
                slot_tick.books.push_back(std::move(patch));

                send.last_bids = view.bids;
                send.last_asks = view.asks;
            }
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
    if (ended_) {
        return false;
    }
    auto step = merge_->Next();
    if (!step) {
        ended_ = true;
        return false;
    }
    clock_.AdvanceTo(step->ts_ns);
    ApplyMergedStep(*step);
    matched_this_pop_ = false;
    return true;
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
    working.price_ticks = ToTicks(order.price, component->tick_size());
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
