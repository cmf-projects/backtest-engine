#pragma once

#include "population/limits.hpp"
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
#include <vector>

namespace population {

// A hole longer than this is a closed session (night, weekend), not a quiet
// stretch of the bar grid. One Advance lands on the next quote. One hour.
inline constexpr std::uint64_t kSessionGapNs = 3'600'000'000'000ULL;

// One Open.components[] row + its tape.
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
    std::size_t index_ = 0;
    double tick_size_ = 0.0;
    std::optional<SnapshotEvent> current_;
    TapeWindow window_;
};

// Bundle population (1…kMaxComponents): one pop_ts, per-component FileBook/TapeWindow,
// impact + working keyed by (slot, component_id).
class Population {
public:
    Population(std::vector<ComponentSpec> components, std::vector<SlotConfig> slots);

    // Single-component convenience (existing tests / P0 call sites).
    Population(std::string component_id, double tick_size, TapePtr tape,
               std::vector<SlotConfig> slots);

    void Start();

    // TZ §5.4: recover impact → match due vs effective copy → patches +
    // trades. The first tick's common book is `Tick.books`; later patches
    // are per slot. Reuses internal scratch; valid until the next
    // MakeTick/Advance/Submit.
    [[nodiscard]] const TickView& MakeTick();

    // Validate + accept into working (effective_ts = pop_ts + Δ). No match.
    // Wire: no failures → OrdersAck ok; else OrdersAck.errors[] (no partial applies).
    // Validates + applies all-or-nothing. Result reused until next Submit.
    [[nodiscard]] const OrderBatchResult& Submit(const OrderBatch& batch);

    [[nodiscard]] bool Advance();

    // -1: each Advance jumps to the next dataset timestamp.
    // > 0: each Advance moves pop_ts by this many nanoseconds. Dataset snaps
    // are applied when pop_ts reaches them; between snaps the book is kept.
    // A hole longer than kSessionGapNs jumps to the next quote instead.
    void SetStepNs(std::int64_t step_ns);

    // No tapes. The session feeds snaps as rows arrive and drops them after expiry.
    explicit Population(std::vector<SlotConfig> slots);
    void StartAt(std::uint64_t ts_ns);
    void AdvanceTo(std::uint64_t ts_ns);
    // Creates the component on the first snap. Later snaps reuse that slot.
    // True when this call allocated the slot.
    bool Observe(const std::string& component_id, double tick_size, const SnapshotEvent& snap);
    // Drops components whose symbol date is strictly before the clock's calendar day.
    void DropExpired(std::uint64_t now_ns);

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

    // Parallel to slots_. send and impact are sized to components_.
    struct SlotState {
        std::vector<ComponentSend> send;
        std::vector<ImpactState> impact;
        SlotOrders orders;
        std::vector<Trade> pending;
    };

    [[nodiscard]] const SlotState* FindSlotState(std::uint32_t slot_id) const;
    [[nodiscard]] SlotState* FindSlotState(std::uint32_t slot_id);

    [[nodiscard]] bool HasSlot(std::uint32_t slot_id) const;
    [[nodiscard]] bool HasComponent(const std::string& component_id) const;
    [[nodiscard]] Component* MutableComponent(const std::string& component_id);
    [[nodiscard]] const SlotConfig* FindSlot(std::uint32_t slot_id) const;
    // nullptr while t - md_lag is before the first snapshot. No warm-up clamp.
    [[nodiscard]] const SnapshotEvent* ViewSnap(const SlotConfig& cfg,
                                                const Component& component) const;
    // False when ViewSnap is empty. Otherwise writes the lagged file book minus
    // this slot's current eaten.
    [[nodiscard]] bool EffectiveViewInto(const SlotConfig& cfg, const Component& component,
                                         const ImpactState& impact, SnapshotEvent& out) const;
    void ApplyMergedStep(const MergedStep& step);
    void TrimWindowForLags();
    void MatchDueIfNeeded();
    // First send, and only when every slot sees the same book: one shared
    // keyframe per component. Records that book as each slot's last send.
    [[nodiscard]] bool SeedSharedFirstTick();

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
    std::vector<SlotState> slot_state_;
    std::uint64_t last_match_pop_ts_ns_ = 0;
    bool matched_this_pop_ = false;

    // Hot-path scratch (reused across MakeTick / match / Advance / Submit).
    MergedStep step_scratch_;
    TickView tick_scratch_;
    SnapshotEvent view_scratch_;
    BookDiffResult diff_scratch_;
    std::vector<Level> match_bids_scratch_;
    std::vector<Level> match_asks_scratch_;
    // Bumped once per Submit that carries cancels. Distinguishes a repeat cancel
    // inside that batch from a cancel in a later batch.
    std::uint32_t cancel_stamp_ = 0;
    // -1 follows dataset timestamps. A positive value is a fixed pop_ts step.
    std::int64_t step_ns_ = -1;
    OrderBatchResult submit_result_;
};

}  // namespace population
