#pragma once

#include "population/clock.hpp"
#include "population/file_book.hpp"
#include "population/impact.hpp"
#include "population/order.hpp"
#include "population/slot.hpp"
#include "population/slot_orders.hpp"
#include "population/tape.hpp"
#include "population/tick.hpp"
#include "population/types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace population {

// Single-component population run (P0): clock + file book + slots → TickView.
class Population {
public:
    Population(std::string component_id, double tick_size, TapePtr tape,
               std::vector<SlotConfig> slots);

    void Start();

    // TZ §5.4: recover impact → match due vs effective copy → md_lag patches +
    // trades. Shared tick.books unused.
    [[nodiscard]] TickView MakeTick();

    // Validate + accept into working (effective_ts = pop_ts + Δ). No match.
    // Wire: no failures → OrdersAck ok; else OrdersAck.errors[] (no partial applies).
    [[nodiscard]] OrderBatchResult Submit(const OrderBatch& batch);

    [[nodiscard]] bool Advance();

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool ended() const noexcept { return book_.ended(); }
    [[nodiscard]] const Clock& clock() const noexcept { return clock_; }
    [[nodiscard]] const FileBook& book() const noexcept { return book_; }
    [[nodiscard]] const std::vector<SlotConfig>& slots() const noexcept { return slots_; }
    [[nodiscard]] const std::string& component_id() const noexcept { return component_id_; }

    [[nodiscard]] const SlotOrders* SlotBook(std::uint32_t slot_id) const;
    [[nodiscard]] const ImpactState* SlotImpact(std::uint32_t slot_id) const;

private:
    struct SlotSendState {
        bool has_sent = false;
        std::vector<Level> last_bids;
        std::vector<Level> last_asks;
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

    [[nodiscard]] bool HasSlot(std::uint32_t slot_id) const;
    [[nodiscard]] const SlotConfig* FindSlot(std::uint32_t slot_id) const;
    [[nodiscard]] SnapshotEvent ViewSnap(const SlotConfig& cfg) const;
    [[nodiscard]] SnapshotEvent EffectiveView(const SlotConfig& cfg) const;
    void TrimWindowForLags();
    void MatchDueIfNeeded();

    [[nodiscard]] OrderResult ValidateNewOrder(const NewOrder& order) const;
    [[nodiscard]] OrderResult ValidateCancel(const CancelOrder& cancel) const;
    [[nodiscard]] OrderResult SubmitOne(const NewOrder& order);
    [[nodiscard]] OrderResult CancelOne(const CancelOrder& cancel);
    [[nodiscard]] long long ToTicks(double price) const;
    [[nodiscard]] bool OnGrid(double price) const;

    std::string component_id_;
    double tick_size_ = 0.0;
    std::vector<SlotConfig> slots_;
    Clock clock_;
    FileBook book_;
    bool started_ = false;
    std::unordered_map<std::uint32_t, SlotSendState> slot_send_;
    std::unordered_map<std::uint32_t, SlotOrders> slot_orders_;
    std::unordered_map<std::uint32_t, ImpactState> slot_impact_;
    std::unordered_map<std::uint32_t, std::vector<Trade>> pending_trades_;
    std::uint64_t last_match_pop_ts_ns_ = 0;
    bool matched_this_pop_ = false;
};

}  // namespace population
