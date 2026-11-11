#pragma once

// Shared little-endian append/read helpers for UDS and SPSC framing.

#include <cstdint>
#include <string>
#include <vector>

namespace transport {

inline void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    const std::size_t n = out.size();
    out.resize(n + 4);
    out[n] = static_cast<std::uint8_t>(v);
    out[n + 1] = static_cast<std::uint8_t>(v >> 8);
    out[n + 2] = static_cast<std::uint8_t>(v >> 16);
    out[n + 3] = static_cast<std::uint8_t>(v >> 24);
}

inline void AppendU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    const std::size_t n = out.size();
    out.resize(n + 8);
    for (int i = 0; i < 8; ++i) {
        out[n + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

[[nodiscard]] inline bool ReadU32(const std::uint8_t*& p, const std::uint8_t* end,
                                  std::uint32_t& v) {
    if (static_cast<std::size_t>(end - p) < 4) {
        return false;
    }
    v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    p += 4;
    return true;
}

[[nodiscard]] inline bool ReadU64(const std::uint8_t*& p, const std::uint8_t* end,
                                  std::uint64_t& v) {
    if (static_cast<std::size_t>(end - p) < 8) {
        return false;
    }
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    p += 8;
    return true;
}

inline void AppendString(std::vector<std::uint8_t>& out, const std::string& s) {
    AppendU32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

[[nodiscard]] inline bool ReadString(const std::uint8_t*& p, const std::uint8_t* end,
                                     std::string& s) {
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

}  // namespace transport
