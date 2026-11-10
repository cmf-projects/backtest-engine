#pragma once

// Venue-side population run over an existing UDS + SPSC pair.
// docs/POPULATION_TZ.md §5.3–5.5; IMPL 2.2 (wires Population).

#include "population/population.hpp"
#include "population/tape.hpp"
#include "transport/bus_messages.hpp"
#include "transport/control.hpp"
#include "transport/queue_registry.hpp"
#include "transport/spsc_bus.hpp"
#include "transport/uds_control.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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
    // Builds a Tape for Open.components[0] (P0 single-component).
    // Default (empty): dataset::MakeJsonlTape(path, sec_code).
    std::function<population::TapePtr(const BusComponent&)> make_tape;
};

class PopulationSession {
public:
    PopulationSession(UdsConn& conn, QueueRegistry& registry, SessionConfig cfg);

    [[nodiscard]] bool valid() const noexcept { return out_.valid() && in_.valid(); }
    [[nodiscard]] SessionPhase phase() const noexcept { return phase_; }

    // Drive SM until Finished. Blocks (polls buses / drain).
    [[nodiscard]] FinishSessionStatus Run();

private:
    using Step = std::optional<FinishSessionStatus>;

    [[nodiscard]] FinishSessionStatus FinishOk(ErrorCode reason, std::string message,
                                               bool close_already_seen);
    [[nodiscard]] FinishSessionStatus FinishFatal(ErrorCode reason, std::string message,
                                                  bool close_already_seen);

    [[nodiscard]] bool SendOut(BusMsgType type, const std::vector<std::uint8_t>& payload);
    [[nodiscard]] BusRecvResult RecvIn(BusMsgType& type, std::vector<std::uint8_t>& payload);

    [[nodiscard]] std::chrono::steady_clock::time_point StallDeadline() const;

    [[nodiscard]] static bool Allowed(SessionPhase phase, BusMsgType type) noexcept;

    [[nodiscard]] Step OnOpen(const std::vector<std::uint8_t>& payload);
    [[nodiscard]] Step OnOrders(const std::vector<std::uint8_t>& payload);
    [[nodiscard]] Step OnAdvance(const std::vector<std::uint8_t>& payload);

    [[nodiscard]] Step SendTickFromPopulation();
    [[nodiscard]] population::TapePtr MakeTape(const BusComponent& component) const;

    UdsConn& conn_;
    QueueRegistry& registry_;
    SessionConfig cfg_;
    SpscBus out_;
    SpscBus in_;
    SessionPhase phase_ = SessionPhase::AwaitOpen;
    std::unique_ptr<population::Population> pop_;
};

}  // namespace transport
