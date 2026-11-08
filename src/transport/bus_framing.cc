#include "transport/bus_framing.hpp"

#include "transport/wire_bytes.hpp"

namespace transport {

std::vector<std::uint8_t> EncodeBusOpen(const BusOpen& msg) {
    std::vector<std::uint8_t> out;
    AppendString(out, msg.population_id);
    return out;
}

bool DecodeBusOpen(const std::uint8_t* data, std::size_t size, BusOpen& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    BusOpen tmp;
    if (!ReadString(p, end, tmp.population_id) || p != end) {
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
    if (!ReadU32(p, end, reason) || !ReadString(p, end, tmp.message) || p != end) {
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
    return out;
}

bool DecodeBusTick(const std::uint8_t* data, std::size_t size, BusTick& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    BusTick tmp;
    if (!ReadU64(p, end, tmp.pop_ts_ns) || p != end) {
        return false;
    }
    out = tmp;
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
