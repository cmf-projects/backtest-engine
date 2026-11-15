#pragma once

#include <atomic>

namespace population_venue {

// Process-wide stop request (SIGINT / SIGTERM). Async-signal-safe store only.
[[nodiscard]] std::atomic<bool>& ShutdownFlag() noexcept;

[[nodiscard]] inline bool StopRequested() noexcept {
    return ShutdownFlag().load(std::memory_order_relaxed);
}

// Install SIGINT + SIGTERM → set ShutdownFlag. Call once from main before Run.
void InstallShutdownHandlers() noexcept;

}  // namespace population_venue
