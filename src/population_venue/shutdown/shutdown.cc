#include "population_venue/shutdown/shutdown.hpp"

#include <csignal>

namespace population_venue {
namespace {

std::atomic<bool> g_shutdown{false};

void OnSignal(int /*signo*/) {
    g_shutdown.store(true, std::memory_order_relaxed);
}

}  // namespace

std::atomic<bool>& ShutdownFlag() noexcept {
    return g_shutdown;
}

void InstallShutdownHandlers() noexcept {
    struct sigaction sa {};
    sa.sa_handler = OnSignal;
    sigemptyset(&sa.sa_mask);
    // No SA_RESTART: blocking accept/poll wake on signal when possible.
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

}  // namespace population_venue
