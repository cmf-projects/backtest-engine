#include "transport/bus_framing.hpp"

#include "transport/wire_bytes.hpp"

namespace transport {
namespace {

void AppendLevels(std::vector<std::uint8_t>& out, const std::vector<BusBookLevel>& levels) {
    AppendU32(out, static_cast<std::uint32_t>(levels.size()));
    for (const auto& lvl : levels) {
        AppendString(out, lvl.price);
        AppendU64(out, lvl.qty);
    }
}

[[nodiscard]] bool ReadLevels(const std::uint8_t*& p, const std::uint8_t* end,
                              std::vector<BusBookLevel>& levels) {
    std::uint32_t n = 0;
    if (!ReadU32(p, end, n)) {
        return false;
    }
    levels.clear();
    levels.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        BusBookLevel lvl;
        if (!ReadString(p, end, lvl.price) || !ReadU64(p, end, lvl.qty)) {
            return false;
        }
        levels.push_back(std::move(lvl));
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
    AppendString(out, trade.price);
    AppendU64(out, trade.qty);
    AppendU64(out, trade.leaves);
}

[[nodiscard]] bool ReadTrade(const std::uint8_t*& p, const std::uint8_t* end, BusTrade& trade) {
    if (!ReadU64(p, end, trade.client_order_id) || p >= end) {
        return false;
    }
    trade.exec_type = static_cast<BusExecType>(*p++);
    return ReadString(p, end, trade.component_id) && ReadString(p, end, trade.price) &&
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

[[nodiscard]] bool ReadSlotTick(const std::uint8_t*& p, const std::uint8_t* end, BusSlotTick& slot) {
    std::uint32_t n_books = 0;
    std::uint32_t n_trades = 0;
    if (!ReadU32(p, end, slot.slot_id) || !ReadU32(p, end, n_books)) {
        return false;
    }
    slot.books.clear();
    slot.books.reserve(n_books);
    for (std::uint32_t i = 0; i < n_books; ++i) {
        BusBookUpdate book;
        if (!ReadBookUpdate(p, end, book)) {
            return false;
        }
        slot.books.push_back(std::move(book));
    }
    if (!ReadU32(p, end, n_trades)) {
        return false;
    }
    slot.trades.clear();
    slot.trades.reserve(n_trades);
    for (std::uint32_t i = 0; i < n_trades; ++i) {
        BusTrade trade;
        if (!ReadTrade(p, end, trade)) {
            return false;
        }
        slot.trades.push_back(std::move(trade));
    }
    return true;
}

}  // namespace

std::vector<std::uint8_t> EncodeBusOpen(const BusOpen& msg) {
    std::vector<std::uint8_t> out;
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

std::vector<std::uint8_t> EncodeBusOpenAck(const BusOpenAck& msg) {
    std::vector<std::uint8_t> out;
    AppendString(out, msg.population_id);
    out.push_back(static_cast<std::uint8_t>(msg.status));
    AppendU32(out, static_cast<std::uint32_t>(msg.reason_code));
    AppendString(out, msg.message);
    AppendU32(out, msg.n_slots);
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

std::vector<std::uint8_t> EncodeBusTick(const BusTick& msg) {
    std::vector<std::uint8_t> out;
    AppendU64(out, msg.pop_ts_ns);
    AppendU32(out, static_cast<std::uint32_t>(msg.books.size()));
    for (const auto& book : msg.books) {
        AppendBookUpdate(out, book);
    }
    AppendU32(out, static_cast<std::uint32_t>(msg.slots.size()));
    for (const auto& slot : msg.slots) {
        AppendSlotTick(out, slot);
    }
    return out;
}

bool DecodeBusTick(const std::uint8_t* data, std::size_t size, BusTick& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    BusTick tmp;
    std::uint32_t n_books = 0;
    std::uint32_t n_slots = 0;
    if (!ReadU64(p, end, tmp.pop_ts_ns) || !ReadU32(p, end, n_books)) {
        return false;
    }
    tmp.books.reserve(n_books);
    for (std::uint32_t i = 0; i < n_books; ++i) {
        BusBookUpdate book;
        if (!ReadBookUpdate(p, end, book)) {
            return false;
        }
        tmp.books.push_back(std::move(book));
    }
    if (!ReadU32(p, end, n_slots)) {
        return false;
    }
    tmp.slots.reserve(n_slots);
    for (std::uint32_t i = 0; i < n_slots; ++i) {
        BusSlotTick slot;
        if (!ReadSlotTick(p, end, slot)) {
            return false;
        }
        tmp.slots.push_back(std::move(slot));
    }
    if (p != end) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

std::vector<std::uint8_t> EncodeBusOrderBatch(const BusOrderBatch& msg) {
    std::vector<std::uint8_t> out;
    AppendU32(out, static_cast<std::uint32_t>(msg.orders.size()));
    for (const auto& o : msg.orders) {
        AppendU32(out, o.slot_id);
        AppendU64(out, o.client_order_id);
        AppendString(out, o.component_id);
        out.push_back(static_cast<std::uint8_t>(o.side));
        AppendString(out, o.price);
        AppendU64(out, o.qty);
        out.push_back(static_cast<std::uint8_t>(o.tif));
    }
    AppendU32(out, static_cast<std::uint32_t>(msg.cancels.size()));
    for (const auto& c : msg.cancels) {
        AppendU32(out, c.slot_id);
        AppendU64(out, c.client_order_id);
    }
    return out;
}

bool DecodeBusOrderBatch(const std::uint8_t* data, std::size_t size, BusOrderBatch& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    BusOrderBatch tmp;
    std::uint32_t n_orders = 0;
    std::uint32_t n_cancels = 0;
    if (!ReadU32(p, end, n_orders)) {
        return false;
    }
    tmp.orders.reserve(n_orders);
    for (std::uint32_t i = 0; i < n_orders; ++i) {
        BusOrder o;
        if (!ReadU32(p, end, o.slot_id) || !ReadU64(p, end, o.client_order_id) ||
            !ReadString(p, end, o.component_id) || p >= end) {
            return false;
        }
        o.side = static_cast<BusSide>(*p++);
        if (!ReadString(p, end, o.price) || !ReadU64(p, end, o.qty) || p >= end) {
            return false;
        }
        o.tif = static_cast<BusTimeInForce>(*p++);
        tmp.orders.push_back(std::move(o));
    }
    if (!ReadU32(p, end, n_cancels)) {
        return false;
    }
    tmp.cancels.reserve(n_cancels);
    for (std::uint32_t i = 0; i < n_cancels; ++i) {
        BusCancel c;
        if (!ReadU32(p, end, c.slot_id) || !ReadU64(p, end, c.client_order_id)) {
            return false;
        }
        tmp.cancels.push_back(std::move(c));
    }
    if (p != end) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

std::vector<std::uint8_t> EncodeBusOrdersAck(const BusOrdersAck& msg) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(msg.status));
    AppendU32(out, static_cast<std::uint32_t>(msg.reason_code));
    AppendString(out, msg.message);
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

std::vector<std::uint8_t> EncodeBusAdvance() {
    return {};
}

bool DecodeBusAdvance(const std::uint8_t* data, std::size_t size) {
    (void)data;
    return size == 0;
}

std::vector<std::uint8_t> EncodeBusEnd() {
    return {};
}

bool DecodeBusEnd(const std::uint8_t* data, std::size_t size) {
    (void)data;
    return size == 0;
}

std::vector<std::uint8_t> EncodeBusClose() {
    return {};
}

bool DecodeBusClose(const std::uint8_t* data, std::size_t size) {
    (void)data;
    return size == 0;
}

std::vector<std::uint8_t> EncodeBusError(const BusError& msg) {
    std::vector<std::uint8_t> out;
    AppendU32(out, static_cast<std::uint32_t>(msg.code));
    AppendString(out, msg.message);
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
