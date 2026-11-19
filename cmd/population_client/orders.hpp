#pragma once

#include "transport/bus_messages.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace client {

struct PendingOrder {
    std::uint64_t submit_pop_ts_ns = 0;
    std::string component_id;
    transport::BusSide side = transport::BusSide::Unspecified;
    transport::BusPrice price;
};

// Live orders only. A finished id is cleared and the slot is reused.
// The table does not grow with client_order_id: the next order always gets a
// higher id, so indexing by oid would keep every dead IOC until the run ends.
struct DensePending {
    struct Slot {
        bool live = false;
        std::uint64_t oid = 0;
        PendingOrder order;
    };

    DensePending() { slots.reserve(8); }

    [[nodiscard]] std::size_t slot_count() const noexcept { return slots.size(); }
    [[nodiscard]] std::size_t live_count() const noexcept { return live_; }

    void Put(std::uint64_t oid, PendingOrder order) {
        if (Slot* existing = FindSlot(oid)) {
            existing->order = std::move(order);
            return;
        }
        for (Slot& slot : slots) {
            if (!slot.live) {
                slot.live = true;
                slot.oid = oid;
                slot.order = std::move(order);
                ++live_;
                return;
            }
        }
        slots.push_back(Slot{.live = true, .oid = oid, .order = std::move(order)});
        ++live_;
    }

    [[nodiscard]] PendingOrder* Find(std::uint64_t oid) {
        Slot* slot = FindSlot(oid);
        return slot == nullptr ? nullptr : &slot->order;
    }

    void Erase(std::uint64_t oid) {
        Slot* slot = FindSlot(oid);
        if (slot == nullptr) {
            return;
        }
        slot->live = false;
        slot->oid = 0;
        slot->order = PendingOrder{};
        --live_;
    }

private:
    [[nodiscard]] Slot* FindSlot(std::uint64_t oid) {
        for (Slot& slot : slots) {
            if (slot.live && slot.oid == oid) {
                return &slot;
            }
        }
        return nullptr;
    }

    std::vector<Slot> slots;
    std::size_t live_ = 0;
};

struct RunStats {
    int ticks = 0;
    int orders_sent = 0;
    int fills = 0;
    int cancels = 0;
    std::uint64_t sum_fill_delay_ns = 0;
    std::uint64_t max_fill_delay_ns = 0;
    std::uint64_t min_fill_delay_ns = UINT64_MAX;
};

void NoteTrades(const transport::BusTick& tick, DensePending& pending, RunStats& stats);

// Last known touch. Quiet ticks omit the book; this keeps bid and ask.
struct RememberedBook {
    std::string component_id;
    bool has_bid = false;
    bool has_ask = false;
    transport::BusPrice bid{};
    transport::BusPrice ask{};
    std::uint64_t bid_qty = 0;
    std::uint64_t ask_qty = 0;
};

// Fold this tick's book patches into `memory`. Every `order_every` ticks, IOC
// one side on each remembered touch: buy the ask, or sell the bid.
void MakeTakeBatch(const transport::BusTick& tick, int tick_n, int order_every,
                   std::uint64_t& next_oid, DensePending& pending, transport::BusOrderBatch& batch,
                   std::vector<RememberedBook>& memory);

}  // namespace client
