#include "transport/spsc_bus.hpp"

#include <cstring>

namespace transport {
namespace {

void WriteU32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

[[nodiscard]] std::uint32_t ReadU32At(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

SpscBus::SpscBus(PopulationQueue* queue) noexcept : queue_(queue) {}

bool SpscBus::PushSlot(BusMsgType type, std::uint8_t flags, std::uint32_t total,
                       std::uint32_t offset, const std::uint8_t* chunk,
                       std::uint32_t chunk_size) {
    if (kBusSlotHeaderBytes + chunk_size > kQueuePayloadBytes) {
        return false;
    }
    QueueMessage slot{};
    WriteU32(slot.bytes + 0, static_cast<std::uint32_t>(type));
    slot.bytes[4] = flags;
    slot.bytes[5] = 0;
    slot.bytes[6] = 0;
    slot.bytes[7] = 0;
    WriteU32(slot.bytes + 8, total);
    WriteU32(slot.bytes + 12, offset);
    WriteU32(slot.bytes + 16, chunk_size);
    if (chunk_size != 0 && chunk != nullptr) {
        std::memcpy(slot.bytes + kBusSlotHeaderBytes, chunk, chunk_size);
    }
    slot.size = static_cast<std::uint32_t>(kBusSlotHeaderBytes + chunk_size);
    return queue_->TryPush(slot);
}

bool SpscBus::TrySend(BusMsgType type, const std::vector<std::uint8_t>& payload) {
    if (queue_ == nullptr) {
        return false;
    }
    if (payload.size() > kMaxLogicalBusBytes) {
        return false;
    }

    const std::uint32_t total = static_cast<std::uint32_t>(payload.size());
    if (total == 0) {
        return PushSlot(type, static_cast<std::uint8_t>(kBusFlagFirst | kBusFlagLast), 0, 0,
                        nullptr, 0);
    }

    std::uint32_t offset = 0;
    bool sent_any = false;
    while (offset < total) {
        const std::uint32_t remaining = total - offset;
        const std::uint32_t chunk =
            remaining > static_cast<std::uint32_t>(kMaxBusChunkBytes)
                ? static_cast<std::uint32_t>(kMaxBusChunkBytes)
                : remaining;
        std::uint8_t flags = 0;
        if (offset == 0) {
            flags |= kBusFlagFirst;
        }
        if (offset + chunk == total) {
            flags |= kBusFlagLast;
        }

        if (!PushSlot(type, flags, total, offset, payload.data() + offset, chunk)) {
            // Under a correct pull SM the out ring is empty while we send one
            // Tick — mid-fail means ring already full / message too wide for
            // free slots (or SM stall). Session layer should hit drain_timeout
            // and SessionEnded{fatal} on UDS; Abort only drops a partial assemble.
            if (sent_any) {
                (void)PushSlot(type, static_cast<std::uint8_t>(kBusFlagAbort | kBusFlagLast), 0, 0,
                               nullptr, 0);
            }
            return false;
        }
        sent_any = true;
        offset += chunk;
    }
    return true;
}

bool SpscBus::TryRecv(BusMsgType& type, std::vector<std::uint8_t>& payload) {
    if (queue_ == nullptr) {
        return false;
    }

    while (true) {
        QueueMessage slot{};
        if (!queue_->TryPop(slot)) {
            return false;
        }
        if (slot.size < kBusSlotHeaderBytes || slot.size > kQueuePayloadBytes) {
            ResetReassembly();
            continue;
        }

        const auto msg_type = static_cast<BusMsgType>(ReadU32At(slot.bytes + 0));
        const std::uint8_t flags = slot.bytes[4];
        const std::uint32_t total = ReadU32At(slot.bytes + 8);
        const std::uint32_t offset = ReadU32At(slot.bytes + 12);
        const std::uint32_t chunk = ReadU32At(slot.bytes + 16);

        if (kBusSlotHeaderBytes + chunk != slot.size) {
            ResetReassembly();
            continue;
        }

        if (flags & kBusFlagAbort) {
            ResetReassembly();
            continue;
        }

        if (flags & kBusFlagFirst) {
            if (total > kMaxLogicalBusBytes) {
                ResetReassembly();
                continue;
            }
            assembling_ = true;
            asm_type_ = msg_type;
            asm_total_ = total;
            asm_buf_.clear();
            asm_buf_.reserve(total);
        } else if (!assembling_ || msg_type != asm_type_ || total != asm_total_) {
            ResetReassembly();
            continue;
        }

        if (offset != asm_buf_.size() || offset + chunk > asm_total_) {
            ResetReassembly();
            continue;
        }

        if (chunk != 0) {
            asm_buf_.insert(asm_buf_.end(), slot.bytes + kBusSlotHeaderBytes,
                            slot.bytes + kBusSlotHeaderBytes + chunk);
        }

        if (flags & kBusFlagLast) {
            if (asm_buf_.size() != asm_total_) {
                ResetReassembly();
                continue;
            }
            type = asm_type_;
            payload = std::move(asm_buf_);
            ResetReassembly();
            return true;
        }
        // Need more fragments; keep assembling_ and pull next slot if available.
    }
}

}  // namespace transport
