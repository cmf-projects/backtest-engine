#pragma once

// Venue-side population run over an existing UDS + SPSC pair (stub population).
// docs/POPULATION_TZ.md §5.3–5.5; IMPL 1.2

#include "transport/bus_messages.hpp"
#include "transport/control.hpp"
#include "transport/queue_registry.hpp"
#include "transport/spsc_bus.hpp"
#include "transport/uds_control.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace transport {

enum class SessionPhase : std::uint8_t {
    AwaitOpen = 1,
    // After Tick: client may send OrderBatch or Advance (or Close).
    AwaitOrderOrAdvance = 2,
    // After ok OrdersAck: only Advance (or Close).
    AwaitAdvance = 3,
    Finished = 4,
};

struct SessionConfig {
    std::string population_id;
    std::uint32_t drain_timeout_ms = kDefaultDrainTimeoutMs;
    // Stub: how many Ticks before End (Advance after the last → End).
    std::uint32_t stub_ticks = 1;
};

class PopulationSession {
public:
    PopulationSession(UdsConn& conn, QueueRegistry& registry, SessionConfig cfg);

    [[nodiscard]] bool valid() const noexcept { return out_.valid() && in_.valid(); }
    [[nodiscard]] SessionPhase phase() const noexcept { return phase_; }

    // Drive stub SM until Finished. Blocks (polls buses / drain).
    [[nodiscard]] FinishSessionStatus Run();

private:
    // nullopt = keep looping; value = session finished with that status.
    using Step = std::optional<FinishSessionStatus>;

    [[nodiscard]] FinishSessionStatus FinishOk(ErrorCode reason, std::string message,
                                               bool close_already_seen);
    [[nodiscard]] FinishSessionStatus FinishFatal(ErrorCode reason, std::string message,
                                                  bool close_already_seen);

    [[nodiscard]] bool SendOut(BusMsgType type, const std::vector<std::uint8_t>& payload);
    [[nodiscard]] BusRecvResult RecvIn(BusMsgType& type, std::vector<std::uint8_t>& payload);

    [[nodiscard]] std::chrono::steady_clock::time_point StallDeadline() const;

    // Protocol gate: is this inbound type legal in the current phase?
    [[nodiscard]] static bool Allowed(SessionPhase phase, BusMsgType type) noexcept;

    [[nodiscard]] Step OnOpen(BusMsgType type, const std::vector<std::uint8_t>& payload);
    [[nodiscard]] Step OnOrderOrAdvance(BusMsgType type, const std::vector<std::uint8_t>& payload);
    [[nodiscard]] Step OnOrders(BusMsgType type, const std::vector<std::uint8_t>& payload);
    [[nodiscard]] Step OnAdvance(BusMsgType type, const std::vector<std::uint8_t>& payload);

    UdsConn& conn_;
    QueueRegistry& registry_;
    SessionConfig cfg_;
    SpscBus out_;
    SpscBus in_;
    SessionPhase phase_ = SessionPhase::AwaitOpen;
    std::uint32_t ticks_sent_ = 0;
    std::uint32_t n_slots_ = 0;
    std::vector<BusSlotConfig> slots_;  // from Open; stub Tick emits one row per slot
};

}  // namespace transport
