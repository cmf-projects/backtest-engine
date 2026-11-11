#include "population_venue/app/app.hpp"

#include "population_venue/shutdown/shutdown.hpp"
#include "transport/control.hpp"
#include "transport/population_session.hpp"
#include "transport/queue_registry.hpp"
#include "transport/uds_control.hpp"
#include "transport/uds_framing.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
    std::fprintf(stderr, "population_venue: session %s done (%s)\n", id.c_str(),
                 FinishName(finished));
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
        std::fprintf(stderr, "population_venue: listen %s (drain_ms=%u)\n", cfg_.uds_path.c_str(),
                     cfg_.drain_timeout_ms);

        auto listener = UdsListener::Bind(cfg_.uds_path);
        if (!listener.valid()) {
            std::fprintf(stderr, "population_venue: bind failed\n");
            return 1;
        }

        std::atomic<bool>& shutdown = ShutdownFlag();

        while (!StopRequested()) {
            std::fprintf(stderr, "population_venue: waiting for engine...\n");
            int raw = -2;
            while (!StopRequested()) {
                raw = listener.AcceptTimeout(kAcceptPollMs);
                if (raw == -2) {
                    continue;  // poll timeout / EINTR
                }
                break;
            }
            if (StopRequested()) {
                std::fprintf(stderr, "population_venue: shutdown — stop accept\n");
                break;
            }
            if (raw < 0) {
                std::fprintf(stderr, "population_venue: accept failed\n");
                return 1;
            }

            UdsConn conn(raw);
            QueueRegistry registry;
            std::vector<std::thread> sessions;
            std::fprintf(stderr, "population_venue: engine connected\n");

            while (conn.valid() && !StopRequested()) {
                auto ack_opt = ServeCreateQueuesPoll(conn, registry, kCreatePollMs);
                if (!ack_opt.has_value()) {
                    continue;  // poll timeout
                }
                const auto& ack = *ack_opt;
                if (ack.status != AckStatus::Ok) {
                    if (ack.population_id.empty()) {
                        std::fprintf(stderr, "population_venue: control EOF or bad create\n");
                        break;
                    }
                    std::fprintf(stderr, "population_venue: create rejected: %s\n",
                                 ack.message.c_str());
                    continue;
                }

                if (StopRequested()) {
                    std::fprintf(stderr,
                                 "population_venue: shutdown — reject new create after stop\n");
                    break;
                }

                std::fprintf(stderr, "population_venue: start population_id=%s\n",
                             ack.population_id.c_str());

                sessions.emplace_back(RunPopulationSession, std::ref(conn), std::ref(registry),
                                      ack.population_id, cfg_.drain_timeout_ms, &shutdown);
            }

            if (StopRequested() && !sessions.empty()) {
                std::fprintf(stderr,
                             "population_venue: shutdown — waiting for %zu session(s)\n",
                             sessions.size());
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

        std::fprintf(stderr, "population_venue: exit 0 (graceful)\n");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "population_venue: %s\n", ex.what());
        return 1;
    }
}

}  // namespace population_venue
