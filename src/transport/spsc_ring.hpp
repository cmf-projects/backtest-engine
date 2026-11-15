#pragma once

// Bounded single-producer / single-consumer ring (POPULATION_IMPL M0.2).
// Layout is mmap-friendly (see shm_queue); one producer + one consumer.
//
// Contract:
// - Exactly one thread (or coroutine) may call TryPush; exactly one may TryPop.
// - T must be trivially copyable.
// - Capacity is fixed at compile time and must be a power of two (>= 2).
// - Full → TryPush false (no overwrite). Empty → TryPop false.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace transport {

void FutexWake(std::uint32_t* addr) noexcept;
void FutexWait(std::uint32_t* addr, std::uint32_t expected,
               std::chrono::steady_clock::time_point deadline) noexcept;

// Fixed like third_party/ipc.h — not std::hardware_destructive_interference_size
// (that value is ABI-unstable across -mtune / toolchains).
inline constexpr std::size_t kCacheLineSize = 64;

// Data: consumer is waiting for a slot to read.
// Space: producer is waiting for a free slot to write.
enum class RingWait : std::uint8_t { Data, Space };

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
        Notify();
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
        Notify();
        return true;
    }

    // Sleep until the peer makes `why` true, or until deadline.
    // Data: caller is the consumer and already observed an empty ring.
    // Space: caller is the producer and already observed a full ring.
    // The wake counter alone is not enough: the push or pop that frees the
    // caller may have bumped it before this samples `seen`. Recheck the ring
    // after arming the sleeper, or that event is slept through until deadline.
    void Wait(std::chrono::steady_clock::time_point deadline, RingWait why) noexcept {
        const std::uint32_t seen = wake_.load(std::memory_order_acquire);
        waiting_.store(1, std::memory_order_release);
        if (ready(why) || wake_.load(std::memory_order_acquire) != seen) {
            waiting_.store(0, std::memory_order_release);
            return;
        }
        FutexWait(reinterpret_cast<std::uint32_t*>(&wake_), seen, deadline);
        waiting_.store(0, std::memory_order_release);
    }

    void Notify() noexcept {
        wake_.fetch_add(1, std::memory_order_release);
        if (waiting_.load(std::memory_order_acquire) != 0) {
            FutexWake(reinterpret_cast<std::uint32_t*>(&wake_));
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    // True when the waiter should not sleep. Called after `waiting_` is published.
    [[nodiscard]] bool ready(RingWait why) const noexcept {
        if (why == RingWait::Data) {
            return !empty();
        }
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        return next != tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t kMask = Capacity - 1;

    // One slot is always left free to distinguish full from empty.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLineSize) std::atomic<std::uint32_t> wake_{0};
    std::atomic<std::uint32_t> waiting_{0};
    T slots_[Capacity]{};
};

}  // namespace transport
