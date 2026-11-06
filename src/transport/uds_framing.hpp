#pragma once

// Length-prefixed UDS control framing for CreateQueues / CreateAck / SessionEnded.
// Layout: u32 type | u32 payload_size | payload bytes (little-endian).

#include "transport/control.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace transport {

enum class ControlMsgType : std::uint32_t {
    CreateQueues = 1,
    CreateAck = 2,
    SessionEnded = 3,
    Close = 4,
};

[[nodiscard]] std::vector<std::uint8_t> EncodeCreateQueues(const CreateQueues& msg);
[[nodiscard]] bool DecodeCreateQueues(const std::uint8_t* data, std::size_t size,
                                      CreateQueues& out);

[[nodiscard]] std::vector<std::uint8_t> EncodeCreateAck(const CreateAck& msg);
[[nodiscard]] bool DecodeCreateAck(const std::uint8_t* data, std::size_t size, CreateAck& out);

[[nodiscard]] std::vector<std::uint8_t> EncodeSessionEnded(const SessionEnded& msg);
[[nodiscard]] bool DecodeSessionEnded(const std::uint8_t* data, std::size_t size,
                                      SessionEnded& out);

[[nodiscard]] std::vector<std::uint8_t> EncodeClose(const Close& msg);
[[nodiscard]] bool DecodeClose(const std::uint8_t* data, std::size_t size, Close& out);

// Full frame = header + payload.
[[nodiscard]] std::vector<std::uint8_t> Frame(ControlMsgType type,
                                              const std::vector<std::uint8_t>& payload);

struct ParsedFrame {
    ControlMsgType type = ControlMsgType::CreateQueues;
    std::vector<std::uint8_t> payload;
};

// Returns false if buffer is incomplete or corrupt. On incomplete, *need_more may be set.
[[nodiscard]] bool TryParseFrame(const std::uint8_t* data, std::size_t size, ParsedFrame& out,
                                 std::size_t* bytes_consumed);

}  // namespace transport
