#include "transport/bus_framing.hpp"

#include "transport/wire_bytes.hpp"

namespace transport {
namespace {

template <typename T>
void ResizeWithPool(std::vector<T>& v, std::uint32_t n, std::vector<T>& pool) {
    while (v.size() > n) {
        pool.push_back(std::move(v.back()));
        v.pop_back();
    }
    while (v.size() < n) {
        if (!pool.empty()) {
            v.push_back(std::move(pool.back()));
            pool.pop_back();
        } else {
            v.emplace_back();
        }
    }
}

void AppendPrice(std::vector<std::uint8_t>& out, const BusPrice& px) {
    AppendI64(out, px.whole);
    AppendI64(out, px.frac);
}

[[nodiscard]] bool ReadPrice(const std::uint8_t*& p, const std::uint8_t* end, BusPrice& px) {
    return ReadI64(p, end, px.whole) && ReadI64(p, end, px.frac);
}

void AppendLevels(std::vector<std::uint8_t>& out, const std::vector<BusBookLevel>& levels) {
    AppendU32(out, static_cast<std::uint32_t>(levels.size()));
    for (const auto& lvl : levels) {
        AppendPrice(out, lvl.price);
        AppendU64(out, lvl.qty);
    }
}

[[nodiscard]] bool ReadLevels(const std::uint8_t*& p, const std::uint8_t* end,
                              std::vector<BusBookLevel>& levels) {
    std::uint32_t n = 0;
    if (!ReadU32(p, end, n)) {
        return false;
    }
    // TZ: max 64 levels/side — pre-reserve so depth changes don't realloc.
    constexpr std::uint32_t kMaxLevels = 64;
    if (n > kMaxLevels) {
        return false;
    }
    if (levels.capacity() < kMaxLevels) {
        levels.reserve(kMaxLevels);
    }
    if (levels.size() != n) {
        levels.resize(n);
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!ReadPrice(p, end, levels[i].price) || !ReadU64(p, end, levels[i].qty)) {
            return false;
        }
    }
    return true;
}

void AppendBookUpdate(std::vector<std::uint8_t>& out, const BusBookUpdate& book) {
    AppendString(out, book.component_id);
    out.push_back(static_cast<std::uint8_t>(book.kind));
    AppendLevels(out, book.bids);
    AppendLevels(out, book.asks);
}

[[nodiscard]] bool ReadBookUpdate(const std::uint8_t*& p, const std::uint8_t* end,
                                  BusBookUpdate& book) {
    if (!ReadString(p, end, book.component_id) || p >= end) {
        return false;
    }
    book.kind = static_cast<BusBookPatchKind>(*p++);
    return ReadLevels(p, end, book.bids) && ReadLevels(p, end, book.asks);
}

void AppendTrade(std::vector<std::uint8_t>& out, const BusTrade& trade) {
    AppendU64(out, trade.client_order_id);
    out.push_back(static_cast<std::uint8_t>(trade.exec_type));
    AppendString(out, trade.component_id);
    AppendPrice(out, trade.price);
    AppendU64(out, trade.qty);
    AppendU64(out, trade.leaves);
}

[[nodiscard]] bool ReadTrade(const std::uint8_t*& p, const std::uint8_t* end, BusTrade& trade) {
    if (!ReadU64(p, end, trade.client_order_id) || p >= end) {
        return false;
    }
    trade.exec_type = static_cast<BusExecType>(*p++);
    return ReadString(p, end, trade.component_id) && ReadPrice(p, end, trade.price) &&
           ReadU64(p, end, trade.qty) && ReadU64(p, end, trade.leaves);
}

void AppendSlotTick(std::vector<std::uint8_t>& out, const BusSlotTick& slot) {
    AppendU32(out, slot.slot_id);
    AppendU32(out, static_cast<std::uint32_t>(slot.books.size()));
    for (const auto& book : slot.books) {
        AppendBookUpdate(out, book);
    }
    AppendU32(out, static_cast<std::uint32_t>(slot.trades.size()));
    for (const auto& trade : slot.trades) {
        AppendTrade(out, trade);
    }
}

[[nodiscard]] bool ReadSlotTick(const std::uint8_t*& p, const std::uint8_t* end, BusSlotTick& slot,
                                BusDecodeScratch& scratch) {
    std::uint32_t n_books = 0;
    std::uint32_t n_trades = 0;
    if (!ReadU32(p, end, slot.slot_id) || !ReadU32(p, end, n_books)) {
        return false;
    }
    ResizeWithPool(slot.books, n_books, scratch.books);
    for (std::uint32_t i = 0; i < n_books; ++i) {
        if (!ReadBookUpdate(p, end, slot.books[i])) {
            return false;
        }
    }
    if (!ReadU32(p, end, n_trades)) {
        return false;
    }
    ResizeWithPool(slot.trades, n_trades, scratch.trades);
    for (std::uint32_t i = 0; i < n_trades; ++i) {
        if (!ReadTrade(p, end, slot.trades[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

void EncodeBusOpenInto(const BusOpen& msg, std::vector<std::uint8_t>& out) {
    out.clear();
    AppendString(out, msg.population_id);
    AppendU32(out, msg.tape_stride);
    AppendU32(out, static_cast<std::uint32_t>(msg.components.size()));
    for (const auto& c : msg.components) {
        AppendString(out, c.component_id);
        out.push_back(static_cast<std::uint8_t>(c.role));
        AppendString(out, c.dataset_path);
        AppendString(out, c.class_code);
        AppendString(out, c.sec_code);
        AppendString(out, c.tick_size);
    }
    AppendU32(out, static_cast<std::uint32_t>(msg.slots.size()));
    for (const auto& s : msg.slots) {
        AppendU32(out, s.slot_id);
        AppendU64(out, s.md_lag_ns);
        AppendU64(out, s.delta_ns);
        AppendString(out, s.impact_lambda);
    }
}

std::vector<std::uint8_t> EncodeBusOpen(const BusOpen& msg) {
    std::vector<std::uint8_t> out;
    EncodeBusOpenInto(msg, out);
    return out;
}

bool DecodeBusOpen(const std::uint8_t* data, std::size_t size, BusOpen& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    BusOpen tmp;
    std::uint32_t n_comp = 0;
    std::uint32_t n_slots = 0;
    if (!ReadString(p, end, tmp.population_id) || !ReadU32(p, end, tmp.tape_stride) ||
        !ReadU32(p, end, n_comp)) {
        return false;
    }
    tmp.components.reserve(n_comp);
    for (std::uint32_t i = 0; i < n_comp; ++i) {
        BusComponent c;
        if (p >= end) {
            return false;
        }
        if (!ReadString(p, end, c.component_id) || p >= end) {
            return false;
        }
        c.role = static_cast<BusAssetRole>(*p++);
        if (!ReadString(p, end, c.dataset_path) || !ReadString(p, end, c.class_code) ||
            !ReadString(p, end, c.sec_code) || !ReadString(p, end, c.tick_size)) {
            return false;
        }
        tmp.components.push_back(std::move(c));
    }
    if (!ReadU32(p, end, n_slots)) {
        return false;
    }
    tmp.slots.reserve(n_slots);
    for (std::uint32_t i = 0; i < n_slots; ++i) {
        BusSlotConfig s;
        if (!ReadU32(p, end, s.slot_id) || !ReadU64(p, end, s.md_lag_ns) ||
            !ReadU64(p, end, s.delta_ns) || !ReadString(p, end, s.impact_lambda)) {
            return false;
        }
        tmp.slots.push_back(std::move(s));
    }
    if (p != end) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

void EncodeBusOpenAckInto(const BusOpenAck& msg, std::vector<std::uint8_t>& out) {
    out.clear();
    AppendString(out, msg.population_id);
    out.push_back(static_cast<std::uint8_t>(msg.status));
    AppendU32(out, static_cast<std::uint32_t>(msg.reason_code));
    AppendString(out, msg.message);
    AppendU32(out, msg.n_slots);
}

std::vector<std::uint8_t> EncodeBusOpenAck(const BusOpenAck& msg) {
    std::vector<std::uint8_t> out;
    EncodeBusOpenAckInto(msg, out);
    return out;
}

bool DecodeBusOpenAck(const std::uint8_t* data, std::size_t size, BusOpenAck& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    BusOpenAck tmp;
    std::uint32_t reason = 0;
    if (!ReadString(p, end, tmp.population_id) || p >= end) {
        return false;
    }
    const std::uint8_t status = *p++;
    if (!ReadU32(p, end, reason) || !ReadString(p, end, tmp.message) ||
        !ReadU32(p, end, tmp.n_slots) || p != end) {
        return false;
    }
    tmp.status = static_cast<AckStatus>(status);
    tmp.reason_code = static_cast<ErrorCode>(reason);
    out = std::move(tmp);
    return true;
}

void EncodeBusTickInto(const BusTick& msg, std::vector<std::uint8_t>& out) {
    out.clear();
    AppendU64(out, msg.pop_ts_ns);
    AppendU32(out, static_cast<std::uint32_t>(msg.books.size()));
    for (const auto& book : msg.books) {
        AppendBookUpdate(out, book);
    }
    AppendU32(out, static_cast<std::uint32_t>(msg.slots.size()));
    for (const auto& slot : msg.slots) {
        AppendSlotTick(out, slot);
    }
}

std::vector<std::uint8_t> EncodeBusTick(const BusTick& msg) {
    std::vector<std::uint8_t> out;
    EncodeBusTickInto(msg, out);
    return out;
}

bool DecodeBusTick(const std::uint8_t* data, std::size_t size, BusTick& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    std::uint32_t n_books = 0;
    std::uint32_t n_slots = 0;
    if (!ReadU64(p, end, out.pop_ts_ns) || !ReadU32(p, end, n_books)) {
        return false;
    }
    ResizeWithPool(out.books, n_books, out.decode_scratch.books);
    for (std::uint32_t i = 0; i < n_books; ++i) {
        if (!ReadBookUpdate(p, end, out.books[i])) {
            return false;
        }
    }
    if (!ReadU32(p, end, n_slots)) {
        return false;
    }
    ResizeWithPool(out.slots, n_slots, out.decode_scratch.slots);
    for (std::uint32_t i = 0; i < n_slots; ++i) {
        if (!ReadSlotTick(p, end, out.slots[i], out.decode_scratch)) {
            return false;
        }
    }
    return p == end;
}

void EncodeBusOrderBatchInto(const BusOrderBatch& msg, std::vector<std::uint8_t>& out) {
    out.clear();
    AppendU32(out, static_cast<std::uint32_t>(msg.orders.size()));
    for (const auto& o : msg.orders) {
        AppendU32(out, o.slot_id);
        AppendU64(out, o.client_order_id);
        AppendString(out, o.component_id);
        out.push_back(static_cast<std::uint8_t>(o.side));
        AppendPrice(out, o.price);
        AppendU64(out, o.qty);
        out.push_back(static_cast<std::uint8_t>(o.tif));
    }
    AppendU32(out, static_cast<std::uint32_t>(msg.cancels.size()));
    for (const auto& c : msg.cancels) {
        AppendU32(out, c.slot_id);
        AppendU64(out, c.client_order_id);
    }
}

std::vector<std::uint8_t> EncodeBusOrderBatch(const BusOrderBatch& msg) {
    std::vector<std::uint8_t> out;
    EncodeBusOrderBatchInto(msg, out);
    return out;
}

bool DecodeBusOrderBatch(const std::uint8_t* data, std::size_t size, BusOrderBatch& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    std::uint32_t n_orders = 0;
    std::uint32_t n_cancels = 0;
    if (!ReadU32(p, end, n_orders)) {
        return false;
    }
    if (out.orders.size() != n_orders) {
        out.orders.resize(n_orders);
    }
    for (std::uint32_t i = 0; i < n_orders; ++i) {
        BusOrder& o = out.orders[i];
        if (!ReadU32(p, end, o.slot_id) || !ReadU64(p, end, o.client_order_id) ||
            !ReadString(p, end, o.component_id) || p >= end) {
            return false;
        }
        o.side = static_cast<BusSide>(*p++);
        if (!ReadPrice(p, end, o.price) || !ReadU64(p, end, o.qty) || p >= end) {
            return false;
        }
        o.tif = static_cast<BusTimeInForce>(*p++);
    }
    if (!ReadU32(p, end, n_cancels)) {
        return false;
    }
    if (out.cancels.size() != n_cancels) {
        out.cancels.resize(n_cancels);
    }
    for (std::uint32_t i = 0; i < n_cancels; ++i) {
        BusCancel& c = out.cancels[i];
        if (!ReadU32(p, end, c.slot_id) || !ReadU64(p, end, c.client_order_id)) {
            return false;
        }
    }
    return p == end;
}

void EncodeBusOrdersAckInto(const BusOrdersAck& msg, std::vector<std::uint8_t>& out) {
    out.clear();
    out.push_back(static_cast<std::uint8_t>(msg.status));
    AppendU32(out, static_cast<std::uint32_t>(msg.reason_code));
    AppendString(out, msg.message);
}

std::vector<std::uint8_t> EncodeBusOrdersAck(const BusOrdersAck& msg) {
    std::vector<std::uint8_t> out;
    EncodeBusOrdersAckInto(msg, out);
    return out;
}

bool DecodeBusOrdersAck(const std::uint8_t* data, std::size_t size, BusOrdersAck& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    BusOrdersAck tmp;
    std::uint32_t reason = 0;
    if (p >= end) {
        return false;
    }
    const std::uint8_t status = *p++;
    if (!ReadU32(p, end, reason) || !ReadString(p, end, tmp.message) || p != end) {
        return false;
    }
    tmp.status = static_cast<AckStatus>(status);
    tmp.reason_code = static_cast<ErrorCode>(reason);
    out = std::move(tmp);
    return true;
}

void EncodeBusAdvanceInto(std::vector<std::uint8_t>& out) { out.clear(); }

std::vector<std::uint8_t> EncodeBusAdvance() { return {}; }

bool DecodeBusAdvance(const std::uint8_t* data, std::size_t size) {
    (void)data;
    return size == 0;
}

void EncodeBusEndInto(std::vector<std::uint8_t>& out) { out.clear(); }

std::vector<std::uint8_t> EncodeBusEnd() { return {}; }

bool DecodeBusEnd(const std::uint8_t* data, std::size_t size) {
    (void)data;
    return size == 0;
}

void EncodeBusCloseInto(std::vector<std::uint8_t>& out) { out.clear(); }

std::vector<std::uint8_t> EncodeBusClose() { return {}; }

bool DecodeBusClose(const std::uint8_t* data, std::size_t size) {
    (void)data;
    return size == 0;
}

void EncodeBusErrorInto(const BusError& msg, std::vector<std::uint8_t>& out) {
    out.clear();
    AppendU32(out, static_cast<std::uint32_t>(msg.code));
    AppendString(out, msg.message);
}

std::vector<std::uint8_t> EncodeBusError(const BusError& msg) {
    std::vector<std::uint8_t> out;
    EncodeBusErrorInto(msg, out);
    return out;
}

bool DecodeBusError(const std::uint8_t* data, std::size_t size, BusError& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    BusError tmp;
    std::uint32_t code = 0;
    if (!ReadU32(p, end, code) || !ReadString(p, end, tmp.message) || p != end) {
        return false;
    }
    tmp.code = static_cast<ErrorCode>(code);
    out = std::move(tmp);
    return true;
}

}  // namespace transport
