#pragma once

// Bounded single-producer / single-consumer ring (POPULATION_IMPL M0.2).
// In-process for now; shm / third_party/ipc.h wiring is a later step.
//
// Contract:
// - Exactly one thread (or coroutine) may call TryPush; exactly one may TryPop.
// - T must be trivially copyable.
// - Capacity is fixed at compile time and must be a power of two (>= 2).
// - Full → TryPush false (no overwrite). Empty → TryPop false.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace transport {

// Fixed like third_party/ipc.h — not std::hardware_destructive_interference_size
// (that value is ABI-unstable across -mtune / toolchains).
inline constexpr std::size_t kCacheLineSize = 64;

template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing: T must be trivially copyable");
    static_assert(Capacity >= 2, "SpscRing: Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "SpscRing: Capacity must be a power of two");

public:
    SpscRing() = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Producer only. Returns false if the ring is full.
    [[nodiscard]] bool TryPush(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer only. Returns false if the ring is empty.
    [[nodiscard]] bool TryPop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // One slot is always left free to distinguish full from empty.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    T slots_[Capacity]{};
};

}  // namespace transport
