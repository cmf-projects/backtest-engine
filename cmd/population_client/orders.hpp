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

// Monotonic client_order_id → slot; no per-insert heap nodes.
struct DensePending {
    struct Slot {
        bool live = false;
        PendingOrder order;
    };

    std::vector<Slot> slots;  // index == oid (0 unused)

    void Reserve(std::size_t n) { slots.reserve(n); }

    void Put(std::uint64_t oid, PendingOrder order) {
        if (oid >= slots.size()) {
            slots.resize(oid + 1);
        }
        slots[oid].order = std::move(order);
        slots[oid].live = true;
    }

    [[nodiscard]] PendingOrder* Find(std::uint64_t oid) {
        if (oid >= slots.size() || !slots[oid].live) {
            return nullptr;
        }
        return &slots[oid].order;
    }

    void Erase(std::uint64_t oid) {
        if (oid < slots.size()) {
            slots[oid].live = false;
        }
    }
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

// Buy the best ask and sell the best bid, both IOC, on every live book.
void MakeTakeBatch(const transport::BusTick& tick, std::uint64_t& next_oid, DensePending& pending,
                   transport::BusOrderBatch& batch,
                   std::vector<const transport::BusBookUpdate*>& books);

}  // namespace client
