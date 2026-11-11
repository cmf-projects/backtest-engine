#pragma once

#include "population/book_diff.hpp"
#include "population/clock.hpp"
#include "population/impact.hpp"
#include "population/merged_tape.hpp"
#include "population/order.hpp"
#include "population/slot.hpp"
#include "population/slot_orders.hpp"
#include "population/tape.hpp"
#include "population/tape_window.hpp"
#include "population/tick.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace population {

// One Open.components[] row + its tape (IMPL 3.2).
struct ComponentSpec {
    std::string component_id;
    double tick_size = 0.0;
    TapePtr tape;
};

// File-book state for one component (window + current snap). Driven by MergedTape.
class Component {
public:
    [[nodiscard]] const std::string& component_id() const noexcept { return component_id_; }
    [[nodiscard]] double tick_size() const noexcept { return tick_size_; }
    [[nodiscard]] bool present() const noexcept { return current_.has_value(); }
    [[nodiscard]] const SnapshotEvent& current() const;
    [[nodiscard]] const TapeWindow& window() const noexcept { return window_; }
    [[nodiscard]] TapeWindow& window() noexcept { return window_; }

private:
    friend class Population;

    std::string component_id_;
    double tick_size_ = 0.0;
    std::optional<SnapshotEvent> current_;
    TapeWindow window_;
};

// Bundle population (1…3 components): one pop_ts, per-component FileBook/TapeWindow,
// impact + working keyed by (slot, component_id).
class Population {
public:
    Population(std::vector<ComponentSpec> components, std::vector<SlotConfig> slots);

    // Single-component convenience (existing tests / P0 call sites).
    Population(std::string component_id, double tick_size, TapePtr tape,
               std::vector<SlotConfig> slots);

    void Start();

    // TZ §5.4: recover impact → match due vs effective copy → md_lag patches +
    // trades. Shared tick.books unused. Reuses internal scratch; valid until next
    // MakeTick/Advance/Submit.
    [[nodiscard]] const TickView& MakeTick();

    // Validate + accept into working (effective_ts = pop_ts + Δ). No match.
    // Wire: no failures → OrdersAck ok; else OrdersAck.errors[] (no partial applies).
    [[nodiscard]] OrderBatchResult Submit(const OrderBatch& batch);

    [[nodiscard]] bool Advance();

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }
    [[nodiscard]] const Clock& clock() const noexcept { return clock_; }
    [[nodiscard]] const std::vector<SlotConfig>& slots() const noexcept { return slots_; }
    [[nodiscard]] std::size_t component_count() const noexcept { return components_.size(); }

    // Sole component when N==1 (compat for older tests).
    [[nodiscard]] const std::string& component_id() const;
    // Sole component when N==1 (compat: was FileBook via book()).
    [[nodiscard]] const Component& book() const;

    // Named Find* so it does not shadow the Component type inside Population.
    [[nodiscard]] const Component* FindComponent(const std::string& component_id) const;

    [[nodiscard]] const SlotOrders* SlotBook(std::uint32_t slot_id) const;
    // Per-slot × component impact (3.2).
    [[nodiscard]] const ImpactState* SlotImpact(std::uint32_t slot_id,
                                                const std::string& component_id) const;
    // N==1 convenience: impact for the sole component.
    [[nodiscard]] const ImpactState* SlotImpact(std::uint32_t slot_id) const;

private:
    struct ComponentSend {
        bool has_sent = false;
        std::vector<Level> last_bids;
        std::vector<Level> last_asks;
    };

    struct SlotSendState {
        // component_id → last sent book for that component
        std::unordered_map<std::string, ComponentSend> components;
    };

    // Batch de-dupe key: (slot_id, client_order_id). Ids are unique per slot, not
    // globally — same client_order_id on two slots is fine.
    using OrderKey = std::pair<std::uint32_t, std::uint64_t>;

    struct OrderKeyHash {
        [[nodiscard]] std::size_t operator()(const OrderKey& key) const noexcept {
            std::size_t h = std::hash<std::uint32_t>{}(key.first);
            // boost::hash_combine(h, client_order_id)
            h ^= std::hash<std::uint64_t>{}(key.second) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    // (slot_id, component_id) → ImpactState
    using SlotComponentKey = std::pair<std::uint32_t, std::string>;

    struct SlotComponentKeyHash {
        [[nodiscard]] std::size_t operator()(const SlotComponentKey& key) const noexcept {
            std::size_t h = std::hash<std::uint32_t>{}(key.first);
            h ^= std::hash<std::string>{}(key.second) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    [[nodiscard]] bool HasSlot(std::uint32_t slot_id) const;
    [[nodiscard]] bool HasComponent(const std::string& component_id) const;
    [[nodiscard]] Component* MutableComponent(const std::string& component_id);
    [[nodiscard]] const SlotConfig* FindSlot(std::uint32_t slot_id) const;
    [[nodiscard]] const SnapshotEvent& ViewSnap(const SlotConfig& cfg,
                                                const Component& component) const;
    void EffectiveViewInto(const SlotConfig& cfg, const Component& component,
                           SnapshotEvent& out) const;
    void ApplyMergedStep(const MergedStep& step);
    void TrimWindowForLags();
    void MatchDueIfNeeded();

    [[nodiscard]] OrderResult ValidateNewOrder(const NewOrder& order) const;
    [[nodiscard]] OrderResult ValidateCancel(const CancelOrder& cancel) const;
    [[nodiscard]] OrderResult SubmitOne(const NewOrder& order);
    [[nodiscard]] OrderResult CancelOne(const CancelOrder& cancel);
    [[nodiscard]] bool OnGrid(double price, double tick_size) const;

    std::vector<Component> components_;
    std::vector<SlotConfig> slots_;
    Clock clock_;
    std::optional<MergedTape> merge_;
    bool started_ = false;
    bool ended_ = false;
    std::unordered_map<std::uint32_t, SlotSendState> slot_send_;
    std::unordered_map<std::uint32_t, SlotOrders> slot_orders_;
    std::unordered_map<SlotComponentKey, ImpactState, SlotComponentKeyHash> slot_impact_;
    std::unordered_map<std::uint32_t, std::vector<Trade>> pending_trades_;
    std::uint64_t last_match_pop_ts_ns_ = 0;
    bool matched_this_pop_ = false;

    // Hot-path scratch (reused across MakeTick / match).
    TickView tick_scratch_;
    SnapshotEvent view_scratch_;
    BookDiffResult diff_scratch_;
    std::vector<Level> match_bids_scratch_;
    std::vector<Level> match_asks_scratch_;
    std::vector<Trade> match_trades_scratch_;
};

}  // namespace population
