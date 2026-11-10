#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace population {

// Side / TimeInForce / ErrorCode (order-scoped; shared with transport wire).
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

// One failed Order/Cancel line — maps to proto `Error` in OrdersAck.errors[].
struct OrderLineFailure {
    std::uint32_t slot_id = 0;
    std::uint64_t client_order_id = 0;
    OrderResult code = OrderResult::Unknown;
};

// Per-line codes from Submit validation. On the wire → OrdersAck:
//   no failures → status=OK, errors empty
//   else        → status=ERROR, errors[] = Failures(batch); batch not applied.
struct OrderBatchResult {
    std::vector<OrderResult> order_results;
    std::vector<OrderResult> cancel_results;

    // Correlates parallel result vectors with the submitted batch.
    [[nodiscard]] std::vector<OrderLineFailure> Failures(
        const OrderBatch& batch) const {
        std::vector<OrderLineFailure> out;
        const std::size_t n_orders =
            std::min(order_results.size(), batch.orders.size());
        for (std::size_t i = 0; i < n_orders; ++i) {
            if (order_results[i] == OrderResult::Ok) {
                continue;
            }
            OrderLineFailure f;
            f.slot_id = batch.orders[i].slot_id;
            f.client_order_id = batch.orders[i].client_order_id;
            f.code = order_results[i];
            out.push_back(f);
        }
        const std::size_t n_cancels =
            std::min(cancel_results.size(), batch.cancels.size());
        for (std::size_t i = 0; i < n_cancels; ++i) {
            if (cancel_results[i] == OrderResult::Ok) {
                continue;
            }
            OrderLineFailure f;
            f.slot_id = batch.cancels[i].slot_id;
            f.client_order_id = batch.cancels[i].client_order_id;
            f.code = cancel_results[i];
            out.push_back(f);
        }
        return out;
    }
};

}  // namespace population
