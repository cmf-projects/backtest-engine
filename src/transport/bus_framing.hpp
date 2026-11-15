#pragma once

#include "transport/bus_messages.hpp"

#include <cstdint>
#include <vector>

namespace transport {

// Encode*Into clears `out` then appends (capacity reused across calls).
// Returning Encode* wrappers allocate a fresh vector (tests / one-shots).

void EncodeBusOpenInto(const BusOpen& msg, std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> EncodeBusOpen(const BusOpen& msg);
[[nodiscard]] bool DecodeBusOpen(const std::uint8_t* data, std::size_t size, BusOpen& out);

void EncodeBusOpenAckInto(const BusOpenAck& msg, std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> EncodeBusOpenAck(const BusOpenAck& msg);
[[nodiscard]] bool DecodeBusOpenAck(const std::uint8_t* data, std::size_t size, BusOpenAck& out);

void EncodeBusTickInto(const BusTick& msg, std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> EncodeBusTick(const BusTick& msg);
[[nodiscard]] bool DecodeBusTick(const std::uint8_t* data, std::size_t size, BusTick& out);

void EncodeBusOrderBatchInto(const BusOrderBatch& msg, std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> EncodeBusOrderBatch(const BusOrderBatch& msg);
[[nodiscard]] bool DecodeBusOrderBatch(const std::uint8_t* data, std::size_t size,
                                       BusOrderBatch& out);

void EncodeBusOrdersAckInto(const BusOrdersAck& msg, std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> EncodeBusOrdersAck(const BusOrdersAck& msg);
[[nodiscard]] bool DecodeBusOrdersAck(const std::uint8_t* data, std::size_t size,
                                      BusOrdersAck& out);

void EncodeBusAdvanceInto(std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> EncodeBusAdvance();
[[nodiscard]] bool DecodeBusAdvance(const std::uint8_t* data, std::size_t size);

void EncodeBusEndInto(std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> EncodeBusEnd();
[[nodiscard]] bool DecodeBusEnd(const std::uint8_t* data, std::size_t size);

void EncodeBusCloseInto(std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> EncodeBusClose();
[[nodiscard]] bool DecodeBusClose(const std::uint8_t* data, std::size_t size);

void EncodeBusErrorInto(const BusError& msg, std::vector<std::uint8_t>& out);
[[nodiscard]] std::vector<std::uint8_t> EncodeBusError(const BusError& msg);
[[nodiscard]] bool DecodeBusError(const std::uint8_t* data, std::size_t size, BusError& out);

}  // namespace transport
