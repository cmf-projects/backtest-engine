#include "population_venue/app/app.hpp"

#include "transport/control.hpp"
#include "transport/population_session.hpp"
#include "transport/queue_registry.hpp"
#include "transport/uds_control.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace population_venue {
namespace {

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
                          std::string population_id, std::uint32_t drain_timeout_ms) {
    transport::SessionConfig session_cfg;
    session_cfg.population_id = population_id;
    session_cfg.drain_timeout_ms = drain_timeout_ms;

    transport::PopulationSession session(conn, registry, std::move(session_cfg));
    const auto finished = session.Run();
    std::fprintf(stderr, "population_venue: session %s done (%s)\n", population_id.c_str(),
                 FinishName(finished));
}

}  // namespace

App::App(Config cfg) : cfg_(std::move(cfg)) {}

int App::Run() {
    using transport::AckStatus;
    using transport::QueueRegistry;
    using transport::ServeCreateQueues;
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

        for (;;) {
            std::fprintf(stderr, "population_venue: waiting for engine...\n");
            const int raw = listener.Accept();
            if (raw < 0) {
                std::fprintf(stderr, "population_venue: accept failed\n");
                return 1;
            }

            UdsConn conn(raw);
            QueueRegistry registry;
            std::vector<std::thread> sessions;
            std::fprintf(stderr, "population_venue: engine connected\n");

            while (conn.valid()) {
                const auto ack = ServeCreateQueues(conn, registry);
                if (ack.status != AckStatus::Ok) {
                    if (ack.population_id.empty()) {
                        std::fprintf(stderr, "population_venue: control EOF or bad create\n");
                        break;
                    }
                    std::fprintf(stderr, "population_venue: create rejected: %s\n",
                                 ack.message.c_str());
                    continue;
                }

                std::fprintf(stderr, "population_venue: start population_id=%s\n",
                             ack.population_id.c_str());

                // One thread per pop: Create does not wait for the previous Run().
                sessions.emplace_back(RunPopulationSession, std::ref(conn), std::ref(registry),
                                      ack.population_id, cfg_.drain_timeout_ms);
            }

            for (auto& t : sessions) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "population_venue: %s\n", ex.what());
        return 1;
    }
}

}  // namespace population_venue
