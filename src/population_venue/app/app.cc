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
#include <optional>
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

void RunPopulationSession(transport::UdsConn& conn, transport::QueueRegistry& registry,
                          std::string population_id, std::uint32_t drain_timeout_ms,
                          std::atomic<bool>* shutdown) {
    const std::string id = population_id;
    transport::SessionConfig session_cfg;
    session_cfg.population_id = std::move(population_id);
    session_cfg.drain_timeout_ms = drain_timeout_ms;
    session_cfg.shutdown = shutdown;

    transport::PopulationSession session(conn, registry, std::move(session_cfg));
    const auto finished = session.Run();
    LOG_INFO("session {} done ({})", id, FinishName(finished));
}

// Like ServeCreateQueues, but returns nullopt on timeout so the app can check stop.
[[nodiscard]] std::optional<transport::CreateAck>
ServeCreateQueuesPoll(transport::UdsConn& conn, transport::QueueRegistry& registry,
                      int poll_ms) {
    using transport::ControlMsgType;
    using transport::CreateQueues;
    using transport::DecodeCreateQueues;
    using transport::EncodeCreateAck;
    using transport::MakeCreateReject;
    using transport::ParsedFrame;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(poll_ms);
    ParsedFrame frame;
    const int got = conn.ReadFrameDeadline(frame, deadline);
    if (got == 0) {
        return std::nullopt;  // timeout — caller retries / checks stop
    }
    if (got < 0) {
        return MakeCreateReject("", transport::ErrorCode::CreateFailed, "control EOF or bad create");
    }

    auto reply = [&](transport::CreateAck ack) {
        (void)conn.WriteFrame(ControlMsgType::CreateAck, EncodeCreateAck(ack));
        return ack;
    };

    if (frame.type != ControlMsgType::CreateQueues) {
        return reply(MakeCreateReject("", transport::ErrorCode::CreateFailed, "bad create frame"));
    }

    CreateQueues req;
    if (!DecodeCreateQueues(frame.payload.data(), frame.payload.size(), req)) {
        return reply(MakeCreateReject("", transport::ErrorCode::CreateFailed, "bad create payload"));
    }
    return reply(registry.Create(req.population_id));
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
            LOG_INFO("engine connected");

            while (conn.valid() && !StopRequested()) {
                auto ack_opt = ServeCreateQueuesPoll(conn, registry, kCreatePollMs);
                if (!ack_opt.has_value()) {
                    continue;  // poll timeout
                }
                const auto& ack = *ack_opt;
                if (ack.status != AckStatus::Ok) {
                    if (ack.population_id.empty()) {
                        LOG_WARN("control EOF or bad create");
                        break;
                    }
                    LOG_WARN("create rejected: {}", ack.message);
                    continue;
                }

                if (StopRequested()) {
                    LOG_INFO("shutdown — reject new create after stop");
                    break;
                }

                LOG_INFO("start population_id={}", ack.population_id);

                sessions.emplace_back(RunPopulationSession, std::ref(conn), std::ref(registry),
                                      ack.population_id, cfg_.drain_timeout_ms, &shutdown);
            }

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
