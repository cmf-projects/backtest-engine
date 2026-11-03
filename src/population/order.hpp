#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace population {

// Mirrors protos/messages.proto Side / TimeInForce / ErrorCode (order-scoped).
enum class Side {
    Unknown = 0,
    Buy,
    Sell,
};

enum class TimeInForce {
    Unknown = 0,
    Day,
    Ioc,
    Fok,
};

enum class OrderResult {
    Unknown = 0,
    Ok,
    UnknownSlot,
    UnknownComponent,
    DuplicateOrderId,
    NoSuchOrder,
    InvalidQty,
    PriceOffGrid,
    NotStarted,
    InvalidSide,
    InvalidTif,
    InvalidClientOrderId,
};

struct NewOrder {
    std::uint32_t slot_id = 0;
    std::uint64_t client_order_id = 0;
    std::string component_id;
    Side side = Side::Unknown;
    double price = 0.0;
    std::uint64_t qty = 0;
    TimeInForce tif = TimeInForce::Unknown;
};

struct CancelOrder {
    std::uint32_t slot_id = 0;
    std::uint64_t client_order_id = 0;
};

// Resting / pending order inside one slot.
struct WorkingOrder {
    std::uint64_t client_order_id = 0;
    std::string component_id;
    Side side = Side::Unknown;
    long long price_ticks = 0;
    std::uint64_t leaves = 0;
    TimeInForce tif = TimeInForce::Unknown;
    // Decision at pop_ts → matchable at pop_ts + slot.Δ (TZ §5.4).
    std::uint64_t effective_ts_ns = 0;
};

enum class ExecType {
    Unknown = 0,
    Fill,
    Cancel,
};

// One exec event for SlotTick.trades (mirrors proto Trade).
struct Trade {
    std::uint64_t client_order_id = 0;
    ExecType exec_type = ExecType::Unknown;
    std::string component_id;
    double price = 0.0;  // fill price; ignored on Cancel
    std::uint64_t qty = 0;
    std::uint64_t leaves = 0;
};

struct OrderBatch {
    std::vector<NewOrder> orders;
    std::vector<CancelOrder> cancels;
};

// Parallel to batch: one reject code per order, then per cancel.
struct OrderBatchResult {
    std::vector<OrderResult> order_results;
    std::vector<OrderResult> cancel_results;
};

}  // namespace population
