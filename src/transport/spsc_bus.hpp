#pragma once

// Typed bus over a PopulationQueue.
// Large payloads are split across slots: First / Cont / Last.
// Slot layout (LE): type u32 | flags u8 | pad3 | total u32 | offset u32 | chunk u32 | bytes
// On full mid-send: return false (no Abort). Session waits drain_timeout then kills the pop.

#include "transport/bus_messages.hpp"
#include "transport/queue_types.hpp"

#include <cstdint>
#include <vector>

namespace transport {

inline constexpr std::uint8_t kBusFlagFirst = 0x01;
inline constexpr std::uint8_t kBusFlagLast = 0x02;

inline constexpr std::size_t kBusSlotHeaderBytes = 20;
inline constexpr std::size_t kMaxBusChunkBytes = kQueuePayloadBytes - kBusSlotHeaderBytes;
// Soft cap so a stuck sender cannot allocate unbounded staging.
inline constexpr std::size_t kMaxLogicalBusBytes = 8u * 1024u * 1024u;

enum class BusRecvResult {
    Empty,          // ring empty (may still be mid-reassembly)
    Message,        // type/payload filled
    ProtocolError,  // corrupt/unexpected fragment; reassembly reset
};

class SpscBus {
public:
    explicit SpscBus(PopulationQueue* queue) noexcept;

    [[nodiscard]] bool valid() const noexcept { return queue_ != nullptr; }

    // Split if needed; false if too large, queue full, or invalid. Does not Abort.
    [[nodiscard]] bool TrySend(BusMsgType type, const std::vector<std::uint8_t>& payload);

    // Reassembles fragments. Protocol violations → ProtocolError (not silent skip).
    [[nodiscard]] BusRecvResult TryRecv(BusMsgType& type, std::vector<std::uint8_t>& payload);

private:
    [[nodiscard]] bool PushSlot(BusMsgType type, std::uint8_t flags, std::uint32_t total,
                                std::uint32_t offset, const std::uint8_t* chunk,
                                std::uint32_t chunk_size);

    void ResetReassembly() {
        assembling_ = false;
        asm_type_ = BusMsgType::Unspecified;
        asm_total_ = 0;
        asm_buf_.clear();
    }

    [[nodiscard]] BusRecvResult FailProtocol() {
        ResetReassembly();
        return BusRecvResult::ProtocolError;
    }

    PopulationQueue* queue_ = nullptr;

    bool assembling_ = false;
    BusMsgType asm_type_ = BusMsgType::Unspecified;
    std::uint32_t asm_total_ = 0;
    std::vector<std::uint8_t> asm_buf_;
};

}  // namespace transport
