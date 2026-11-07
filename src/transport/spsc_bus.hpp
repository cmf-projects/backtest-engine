#pragma once

// Typed bus over a PopulationQueue.
// Large payloads are split across slots: First / Cont / Last (+ Abort on failed send).
// Slot layout (LE): type u32 | flags u8 | pad3 | total u32 | offset u32 | chunk u32 | bytes

#include "transport/bus_messages.hpp"
#include "transport/queue_registry.hpp"

#include <cstdint>
#include <vector>

namespace transport {

inline constexpr std::uint8_t kBusFlagFirst = 0x01;
inline constexpr std::uint8_t kBusFlagLast = 0x02;
inline constexpr std::uint8_t kBusFlagAbort = 0x04;

inline constexpr std::size_t kBusSlotHeaderBytes = 20;
inline constexpr std::size_t kMaxBusChunkBytes = kQueuePayloadBytes - kBusSlotHeaderBytes;
// Soft cap so a stuck sender cannot allocate unbounded staging.
inline constexpr std::size_t kMaxLogicalBusBytes = 8u * 1024u * 1024u;

class SpscBus {
public:
    explicit SpscBus(PopulationQueue* queue) noexcept;

    [[nodiscard]] bool valid() const noexcept { return queue_ != nullptr; }

    // Split if needed; false if too large, queue full mid-send, or invalid.
    [[nodiscard]] bool TrySend(BusMsgType type, const std::vector<std::uint8_t>& payload);

    // Reassembles fragments; false if no complete message yet (or empty).
    [[nodiscard]] bool TryRecv(BusMsgType& type, std::vector<std::uint8_t>& payload);

private:
    [[nodiscard]] bool PushSlot(BusMsgType type, std::uint8_t flags, std::uint32_t total,
                                std::uint32_t offset, const std::uint8_t* chunk,
                                std::uint32_t chunk_size);

    void ResetReassembly() {
        assembling_ = false;
        asm_type_ = BusMsgType::Open;
        asm_total_ = 0;
        asm_buf_.clear();
    }

    PopulationQueue* queue_ = nullptr;

    bool assembling_ = false;
    BusMsgType asm_type_ = BusMsgType::Open;
    std::uint32_t asm_total_ = 0;
    std::vector<std::uint8_t> asm_buf_;
};

}  // namespace transport
