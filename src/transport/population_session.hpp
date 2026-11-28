#pragma once

// Venue-side population run over an existing UDS + SPSC pair.
// docs/POPULATION_TZ.md §5.3–5.5; IMPL 2.2 (wires Population).

#include "dataset/feather_tape.hpp"
#include "population/population.hpp"
#include "population/tape.hpp"
#include "transport/bus_messages.hpp"
#include "transport/bus_population_map.hpp"
#include "transport/control.hpp"
#include "transport/queue_registry.hpp"
#include "transport/spsc_bus.hpp"
#include "transport/uds_control.hpp"

#include <atomic>
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
    // Builds a Tape for each Open.components[] row (1…3).
    // Default (empty): dataset::MakeTape(path, sec_code) — JSONL or Feather by path.
    std::function<population::TapePtr(const BusComponent&)> make_tape;
    // Optional process stop flag (Ctrl+C). When set, Run() FinishFatal("venue shutdown").
    std::atomic<bool>* shutdown = nullptr;
    // This control connection is gone (peer closed the UDS). Run() finishes
    // immediately instead of waiting out drain_timeout.
    std::atomic<bool>* conn_stop = nullptr;
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
    [[nodiscard]] bool AdvanceJournals();
    void DrainJournals(std::uint64_t now_ns);

    struct JournalHead {
        std::unique_ptr<dataset::FeatherJournal> journal;
        std::string symbol;
        population::SnapshotEvent event;
        double tick = 0.0;
        bool ok = false;
    };

    UdsConn& conn_;
    QueueRegistry& registry_;
    SessionConfig cfg_;
    SpscBus out_;
    SpscBus in_;
    SessionPhase phase_ = SessionPhase::AwaitOpen;
    std::unique_ptr<population::Population> pop_;
    std::vector<std::uint8_t> encode_buf_;  // reused by Encode*Into
    std::vector<std::uint8_t> recv_buf_;    // reused by TryRecv
    BusTick tick_buf_;                      // reused by ToBusTickInto
    BusOrderBatch order_batch_buf_;         // reused by OnOrders decode
    population::OrderBatch domain_batch_;   // reused by ToOrderBatchInto
    TickSizeByComponent tick_sizes_;        // from Open.components; wire price grid
    std::vector<JournalHead> journals_;
    std::int64_t step_ns_ = -1;
};

}  // namespace transport
