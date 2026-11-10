#pragma once

// Logical messages on the population SPSC pair (not UDS).
// docs/POPULATION_TZ.md §5.2–5.3; shapes mirror protos/messages.proto

#include "transport/control.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace transport {

enum class BusMsgType : std::uint32_t {
    Unspecified = 0,  // idle / invalid; never on the wire as a real message
    Open = 1,         // engine → venue (in)
    OpenAck = 2,      // venue → engine (out)
    Tick = 3,         // venue → engine (out)
    OrderBatch = 4,   // engine → venue (in)
    OrdersAck = 5,    // venue → engine (out)
    Advance = 6,      // engine → venue (in)
    End = 7,          // venue → engine (out)
    Error = 8,        // venue → engine (out) fatal
    Close = 9,        // engine → venue (in)
};

enum class BusAssetRole : std::uint8_t {
    Unspecified = 0,
    Spot = 1,
    Future = 2,
    Option = 3,
};

enum class BusBookPatchKind : std::uint8_t {
    Unspecified = 0,
    Keyframe = 1,
    Diff = 2,
};

enum class BusExecType : std::uint8_t {
    Unspecified = 0,
    Fill = 1,
    Cancel = 2,
};

enum class BusSide : std::uint8_t {
    Unspecified = 0,
    Buy = 1,
    Sell = 2,
};

enum class BusTimeInForce : std::uint8_t {
    Unspecified = 0,
    Day = 1,
    Ioc = 2,
    Fok = 3,
};

struct BusComponent {
    std::string component_id;
    BusAssetRole role = BusAssetRole::Unspecified;
    std::string dataset_path;
    std::string class_code;
    std::string sec_code;
    std::string tick_size;  // decimal text, e.g. "0.01"
};

struct BusSlotConfig {
    std::uint32_t slot_id = 0;
    std::uint64_t md_lag_ns = 0;
    std::uint64_t delta_ns = 0;       // MUST be >= 1
    std::string impact_lambda;       // decimal text; "0" = impact off
};

// Engine → venue: open population on already-created queues.
struct BusOpen {
    std::string population_id;
    std::uint32_t tape_stride = 1;  // 0 or 1 → one event per tick
    std::vector<BusComponent> components;
    std::vector<BusSlotConfig> slots;
};

struct BusOpenAck {
    std::string population_id;
    AckStatus status = AckStatus::Unspecified;
    ErrorCode reason_code = ErrorCode::Unspecified;
    std::string message;
    // Echoed on OK: how many slot rows to expect per Tick.
    std::uint32_t n_slots = 0;
};

struct BusBookLevel {
    std::string price;  // decimal text
    std::uint64_t qty = 0;
};

struct BusBookUpdate {
    std::string component_id;
    BusBookPatchKind kind = BusBookPatchKind::Unspecified;
    std::vector<BusBookLevel> bids;
    std::vector<BusBookLevel> asks;
};

struct BusTrade {
    std::uint64_t client_order_id = 0;
    BusExecType exec_type = BusExecType::Unspecified;
    std::string component_id;
    std::string price;  // fill price; empty on Cancel
    std::uint64_t qty = 0;
    std::uint64_t leaves = 0;
};

struct BusSlotTick {
    std::uint32_t slot_id = 0;
    std::vector<BusBookUpdate> books;
    std::vector<BusTrade> trades;
};

struct BusTick {
    std::uint64_t pop_ts_ns = 0;
    std::vector<BusBookUpdate> books;  // shared seed (often empty with per-slot md_lag)
    std::vector<BusSlotTick> slots;
};

struct BusOrder {
    std::uint32_t slot_id = 0;
    std::uint64_t client_order_id = 0;
    std::string component_id;
    BusSide side = BusSide::Unspecified;
    std::string price;  // decimal text
    std::uint64_t qty = 0;
    BusTimeInForce tif = BusTimeInForce::Unspecified;
};

struct BusCancel {
    std::uint32_t slot_id = 0;
    std::uint64_t client_order_id = 0;
};

// Engine → venue after Tick (may be empty).
struct BusOrderBatch {
    std::vector<BusOrder> orders;
    std::vector<BusCancel> cancels;
};

struct BusOrdersAck {
    AckStatus status = AckStatus::Unspecified;
    ErrorCode reason_code = ErrorCode::Unspecified;
    std::string message;
};

struct BusAdvance {};

struct BusEnd {};

struct BusClose {};  // empty; population is implied by the SPSC pair

struct BusError {
    ErrorCode code = ErrorCode::Unspecified;
    std::string message;
};

[[nodiscard]] inline BusOpenAck MakeBusOpenOk(std::string population_id, std::uint32_t n_slots) {
    BusOpenAck ack;
    ack.population_id = std::move(population_id);
    ack.status = AckStatus::Ok;
    ack.n_slots = n_slots;
    return ack;
}

[[nodiscard]] inline BusOpenAck MakeBusOpenReject(std::string population_id, ErrorCode code,
                                                  std::string message) {
    BusOpenAck ack;
    ack.population_id = std::move(population_id);
    ack.status = AckStatus::Error;
    ack.reason_code = code;
    ack.message = std::move(message);
    ack.n_slots = 0;
    return ack;
}

[[nodiscard]] inline BusOrdersAck MakeOrdersAckOk() {
    BusOrdersAck ack;
    ack.status = AckStatus::Ok;
    return ack;
}

[[nodiscard]] inline BusOrdersAck MakeOrdersAckReject(ErrorCode code, std::string message) {
    BusOrdersAck ack;
    ack.status = AckStatus::Error;
    ack.reason_code = code;
    ack.message = std::move(message);
    return ack;
}

// Lightweight Open checks before Start (duplicate ids, Δ=0, empty bundle).
[[nodiscard]] inline bool ValidateBusOpen(const BusOpen& open, std::string& why) {
    if (open.population_id.empty()) {
        why = "empty population_id";
        return false;
    }
    if (open.components.empty()) {
        why = "no components";
        return false;
    }
    if (open.slots.empty()) {
        why = "no slots";
        return false;
    }
    for (std::size_t i = 0; i < open.components.size(); ++i) {
        const auto& c = open.components[i];
        if (c.component_id.empty() || c.role == BusAssetRole::Unspecified ||
            c.dataset_path.empty() || c.tick_size.empty()) {
            why = "bad component";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (open.components[j].component_id == c.component_id) {
                why = "duplicate component_id";
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < open.slots.size(); ++i) {
        const auto& s = open.slots[i];
        if (s.delta_ns == 0) {
            why = "delta_ns == 0";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (open.slots[j].slot_id == s.slot_id) {
                why = "duplicate slot_id";
                return false;
            }
        }
    }
    why.clear();
    return true;
}

}  // namespace transport
