#include "transport/uds_framing.hpp"

#include <cstring>

namespace transport {
namespace {

constexpr std::size_t kHeaderSize = 8;  // type + payload_size

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

[[nodiscard]] bool ReadU32(const std::uint8_t*& p, const std::uint8_t* end, std::uint32_t& v) {
    if (static_cast<std::size_t>(end - p) < 4) {
        return false;
    }
    v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    p += 4;
    return true;
}

void AppendString(std::vector<std::uint8_t>& out, const std::string& s) {
    AppendU32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

[[nodiscard]] bool ReadString(const std::uint8_t*& p, const std::uint8_t* end, std::string& s) {
    std::uint32_t n = 0;
    if (!ReadU32(p, end, n)) {
        return false;
    }
    if (static_cast<std::size_t>(end - p) < n) {
        return false;
    }
    s.assign(reinterpret_cast<const char*>(p), n);
    p += n;
    return true;
}

}  // namespace

std::vector<std::uint8_t> EncodeCreateQueues(const CreateQueues& msg) {
    std::vector<std::uint8_t> out;
    AppendString(out, msg.population_id);
    return out;
}

bool DecodeCreateQueues(const std::uint8_t* data, std::size_t size, CreateQueues& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    CreateQueues tmp;
    if (!ReadString(p, end, tmp.population_id) || p != end) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

std::vector<std::uint8_t> EncodeCreateAck(const CreateAck& msg) {
    std::vector<std::uint8_t> out;
    AppendString(out, msg.population_id);
    out.push_back(static_cast<std::uint8_t>(msg.status));
    AppendU32(out, static_cast<std::uint32_t>(msg.reason_code));
    AppendString(out, msg.message);
    AppendString(out, msg.out_queue);
    AppendString(out, msg.in_queue);
    return out;
}

bool DecodeCreateAck(const std::uint8_t* data, std::size_t size, CreateAck& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    CreateAck tmp;
    std::uint32_t reason = 0;
    std::uint8_t status = 0;
    if (!ReadString(p, end, tmp.population_id)) {
        return false;
    }
    if (p >= end) {
        return false;
    }
    status = *p++;
    if (!ReadU32(p, end, reason) || !ReadString(p, end, tmp.message) ||
        !ReadString(p, end, tmp.out_queue) || !ReadString(p, end, tmp.in_queue) || p != end) {
        return false;
    }
    tmp.status = static_cast<AckStatus>(status);
    tmp.reason_code = static_cast<ErrorCode>(reason);
    out = std::move(tmp);
    return true;
}

std::vector<std::uint8_t> EncodeSessionEnded(const SessionEnded& msg) {
    std::vector<std::uint8_t> out;
    AppendString(out, msg.population_id);
    out.push_back(static_cast<std::uint8_t>(msg.status));
    AppendU32(out, static_cast<std::uint32_t>(msg.reason_code));
    AppendString(out, msg.message);
    return out;
}

bool DecodeSessionEnded(const std::uint8_t* data, std::size_t size, SessionEnded& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    SessionEnded tmp;
    std::uint32_t reason = 0;
    if (!ReadString(p, end, tmp.population_id) || p >= end) {
        return false;
    }
    const std::uint8_t status = *p++;
    if (!ReadU32(p, end, reason) || !ReadString(p, end, tmp.message) || p != end) {
        return false;
    }
    tmp.status = static_cast<SessionEndStatus>(status);
    tmp.reason_code = static_cast<ErrorCode>(reason);
    out = std::move(tmp);
    return true;
}

std::vector<std::uint8_t> EncodeClose(const Close& msg) {
    std::vector<std::uint8_t> out;
    AppendString(out, msg.population_id);
    return out;
}

bool DecodeClose(const std::uint8_t* data, std::size_t size, Close& out) {
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    Close tmp;
    if (!ReadString(p, end, tmp.population_id) || p != end) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

std::vector<std::uint8_t> Frame(ControlMsgType type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload.size());
    AppendU32(out, static_cast<std::uint32_t>(type));
    AppendU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool TryParseFrame(const std::uint8_t* data, std::size_t size, ParsedFrame& out,
                   std::size_t* bytes_consumed) {
    if (bytes_consumed != nullptr) {
        *bytes_consumed = 0;
    }
    if (size < kHeaderSize) {
        return false;
    }
    const std::uint8_t* p = data;
    const std::uint8_t* end = data + size;
    std::uint32_t type = 0;
    std::uint32_t payload_size = 0;
    if (!ReadU32(p, end, type) || !ReadU32(p, end, payload_size)) {
        return false;
    }
    if (static_cast<std::size_t>(end - p) < payload_size) {
        return false;
    }
    out.type = static_cast<ControlMsgType>(type);
    out.payload.assign(p, p + payload_size);
    if (bytes_consumed != nullptr) {
        *bytes_consumed = kHeaderSize + payload_size;
    }
    return true;
}

}  // namespace transport
