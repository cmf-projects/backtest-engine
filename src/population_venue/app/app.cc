#include "population_venue/app/app.hpp"

#include "population_venue/log/log.hpp"
#include "population_venue/shutdown/shutdown.hpp"
#include "transport/control.hpp"
#include "transport/population_session.hpp"
#include "transport/queue_registry.hpp"
#include "transport/uds_control.hpp"
#include "transport/uds_framing.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace population_venue {
namespace {

constexpr int kAcceptPollMs = 200;
constexpr int kCreatePollMs = 200;

[[nodiscard]] const char* FinishName(transport::FinishSessionStatus st) {
    using transport::FinishSessionStatus;
    switch (st) {
        case FinishSessionStatus::ClosedByClient:
            return "closed_by_client";
        case FinishSessionStatus::TimedOut:
            return "timed_out";
        case FinishSessionStatus::Failed:
            return "failed";
        case FinishSessionStatus::Unspecified:
            return "unspecified";
    }
    return "unknown";
}

void AbortPopulationSession(transport::UdsConn& conn, transport::QueueRegistry& registry,
                            const std::string& id, const char* what) {
    LOG_ERROR("session {} aborted: {}", id, what);
    try {
        (void)registry.Destroy(id);
        (void)conn.WriteFrame(transport::ControlMsgType::SessionEnded,
                              transport::EncodeSessionEnded(transport::MakeSessionEndedFatal(
                                  id, transport::ErrorCode::Internal, what)));
    } catch (const std::exception& ex) {
        LOG_ERROR("session {} cleanup failed: {}", id, ex.what());
    } catch (...) {
        LOG_ERROR("session {} cleanup failed", id);
    }
}

void RunPopulationSession(transport::UdsConn& conn, transport::QueueRegistry& registry,
                          std::string population_id, std::uint32_t drain_timeout_ms,
                          std::atomic<bool>* shutdown, std::atomic<bool>* conn_stop) {
    const std::string id = population_id;
    try {
        transport::SessionConfig session_cfg;
        session_cfg.population_id = std::move(population_id);
        session_cfg.drain_timeout_ms = drain_timeout_ms;
        session_cfg.shutdown = shutdown;
        session_cfg.conn_stop = conn_stop;

        transport::PopulationSession session(conn, registry, std::move(session_cfg));
        const auto finished = session.Run();
        LOG_INFO("session {} done ({})", id, FinishName(finished));
    } catch (const std::exception& ex) {
        AbortPopulationSession(conn, registry, id, ex.what());
    } catch (...) {
        AbortPopulationSession(conn, registry, id, "unknown exception");
    }
}

}  // namespace

App::App(Config cfg) : cfg_(std::move(cfg)) {}

int App::Run() {
    using transport::AckStatus;
    using transport::QueueRegistry;
    using transport::UdsConn;
    using transport::UdsListener;

    try {
        LOG_INFO("listen {} (drain_ms={})", cfg_.uds_path, cfg_.drain_timeout_ms);
        transport::ReapStaleShmQueues(/*include_self=*/true);

        auto listener = UdsListener::Bind(cfg_.uds_path);
        if (!listener.valid()) {
            LOG_ERROR("bind failed");
            return 1;
        }

        std::atomic<bool>& shutdown = ShutdownFlag();

        while (!StopRequested()) {
            LOG_INFO("waiting for engine...");
            int raw = -2;
            while (!StopRequested()) {
                raw = listener.AcceptTimeout(kAcceptPollMs);
                if (raw == -2) {
                    continue;  // poll timeout / EINTR
                }
                break;
            }
            if (StopRequested()) {
                LOG_INFO("shutdown — stop accept");
                break;
            }
            if (raw < 0) {
                LOG_ERROR("accept failed");
                return 1;
            }

            UdsConn conn(raw);
            QueueRegistry registry;
            std::vector<std::thread> sessions;
            std::atomic<bool> conn_stop{false};
            LOG_INFO("engine connected");

            while (conn.valid() && !StopRequested()) {
                const auto served = transport::ServeCreateQueuesOnce(
                    conn, registry,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(kCreatePollMs));
                if (served.status == transport::CreateServeStatus::Timeout) {
                    continue;
                }
                if (served.status == transport::CreateServeStatus::Eof) {
                    LOG_WARN("control EOF");
                    break;
                }
                const auto& ack = served.ack;
                if (ack.status != AckStatus::Ok) {
                    LOG_WARN("create rejected: code={} {}",
                             static_cast<unsigned>(ack.reason_code), ack.message);
                    continue;
                }

                if (StopRequested()) {
                    LOG_INFO("shutdown — reject new create after stop");
                    break;
                }

                LOG_INFO("start population_id={}", ack.population_id);

                sessions.emplace_back(RunPopulationSession, std::ref(conn), std::ref(registry),
                                      ack.population_id, cfg_.drain_timeout_ms, &shutdown,
                                      &conn_stop);
            }

            // Control socket is done (EOF or venue stop). Wake every session of
            // this connection so they do not sit out drain_timeout.
            conn_stop.store(true, std::memory_order_relaxed);
            registry.WakeAll();

            if (StopRequested() && !sessions.empty()) {
                LOG_INFO("shutdown — waiting for {} session(s)", sessions.size());
            }

            for (auto& t : sessions) {
                if (t.joinable()) {
                    t.join();
                }
            }

            if (StopRequested()) {
                break;
            }
        }

        LOG_INFO("exit 0 (graceful)");
        return 0;
    } catch (const std::exception& ex) {
        LOG_ERROR("{}", ex.what());
        return 1;
    }
}

}  // namespace population_venue
