#pragma once

// Shared SPSC slot types (heap or POSIX shm). Kept separate so shm_queue
// does not pull in QueueRegistry.

#include "transport/spsc_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace transport {

inline constexpr std::size_t kQueueCapacity = 1024;
// Bytes per ring slot body; logical bus messages may span many slots.
inline constexpr std::size_t kQueuePayloadBytes = 2040;

struct QueueMessage {
    std::uint32_t size = 0;
    std::uint8_t bytes[kQueuePayloadBytes]{};
};

static_assert(std::is_trivially_copyable_v<QueueMessage>);

using PopulationQueue = SpscRing<QueueMessage, kQueueCapacity>;

}  // namespace transport
