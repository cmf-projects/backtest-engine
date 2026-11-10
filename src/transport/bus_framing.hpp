#pragma once

#include "transport/bus_messages.hpp"

#include <cstdint>
#include <vector>

namespace transport {

[[nodiscard]] std::vector<std::uint8_t> EncodeBusOpen(const BusOpen& msg);
[[nodiscard]] bool DecodeBusOpen(const std::uint8_t* data, std::size_t size, BusOpen& out);

[[nodiscard]] std::vector<std::uint8_t> EncodeBusOpenAck(const BusOpenAck& msg);
[[nodiscard]] bool DecodeBusOpenAck(const std::uint8_t* data, std::size_t size, BusOpenAck& out);

[[nodiscard]] std::vector<std::uint8_t> EncodeBusTick(const BusTick& msg);
[[nodiscard]] bool DecodeBusTick(const std::uint8_t* data, std::size_t size, BusTick& out);

[[nodiscard]] std::vector<std::uint8_t> EncodeBusOrderBatch(const BusOrderBatch& msg);
[[nodiscard]] bool DecodeBusOrderBatch(const std::uint8_t* data, std::size_t size,
                                       BusOrderBatch& out);

[[nodiscard]] std::vector<std::uint8_t> EncodeBusOrdersAck(const BusOrdersAck& msg);
[[nodiscard]] bool DecodeBusOrdersAck(const std::uint8_t* data, std::size_t size,
                                      BusOrdersAck& out);

[[nodiscard]] std::vector<std::uint8_t> EncodeBusAdvance();
[[nodiscard]] bool DecodeBusAdvance(const std::uint8_t* data, std::size_t size);

[[nodiscard]] std::vector<std::uint8_t> EncodeBusEnd();
[[nodiscard]] bool DecodeBusEnd(const std::uint8_t* data, std::size_t size);

[[nodiscard]] std::vector<std::uint8_t> EncodeBusClose();
[[nodiscard]] bool DecodeBusClose(const std::uint8_t* data, std::size_t size);

[[nodiscard]] std::vector<std::uint8_t> EncodeBusError(const BusError& msg);
[[nodiscard]] bool DecodeBusError(const std::uint8_t* data, std::size_t size, BusError& out);

}  // namespace transport
