// Same-process smoke: UDS control + shared in-process SPSC (shm not ready yet).
// Usage: population_smoke [dataset.jsonl] [uds_path]

#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/control.hpp"
#include "transport/population_session.hpp"
#include "transport/queue_registry.hpp"
#include "transport/spsc_bus.hpp"
#include "transport/uds_control.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using transport::AckStatus;
using transport::BusAssetRole;
using transport::BusComponent;
using transport::BusMsgType;
using transport::BusOpen;
using transport::BusOpenAck;
using transport::BusRecvResult;
using transport::BusSlotConfig;
using transport::BusTick;
using transport::CreateQueues;
using transport::DecodeBusEnd;
using transport::DecodeBusOpenAck;
using transport::DecodeBusTick;
using transport::EncodeBusAdvance;
using transport::EncodeBusClose;
using transport::EncodeBusOpen;
using transport::FinishSessionStatus;
using transport::PopulationSession;
using transport::QueueRegistry;
using transport::RequestCreateQueues;
using transport::ServeCreateQueues;
using transport::SessionConfig;
using transport::SessionEnded;
using transport::SpscBus;
using transport::UdsConn;
using transport::UdsConnect;
using transport::UdsListener;
using transport::WaitSessionEnded;

bool RecvMessage(SpscBus& bus, BusMsgType& type, std::vector<std::uint8_t>& payload,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto r = bus.TryRecv(type, payload);
        if (r == BusRecvResult::Message) {
            return true;
        }
        if (r == BusRecvResult::ProtocolError) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

BusOpen MakeOpen(const std::string& population_id, const std::string& dataset) {
    BusOpen open;
    open.population_id = population_id;
    open.components.push_back(BusComponent{
        .component_id = "SPOT",
        .role = BusAssetRole::Spot,
        .dataset_path = dataset,
        .class_code = "TQBR",
        .sec_code = "SBER",
        .tick_size = "0.01",
    });
    open.slots.push_back(BusSlotConfig{
        .slot_id = 1,
        .md_lag_ns = 0,
        .delta_ns = 1,
        .impact_lambda = "0",
    });
    return open;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dataset =
        (argc >= 2 && argv[1][0] != '\0') ? argv[1] : "testdata/population/smoke.jsonl";
    const std::string uds_path =
        (argc >= 3 && argv[2][0] != '\0') ? argv[2]
        : (std::getenv("POPULATION_UDS") != nullptr ? std::getenv("POPULATION_UDS")
                                                    : "/tmp/cmf-population-smoke.sock");

    FinishSessionStatus status = FinishSessionStatus::Failed;
    SessionEnded ended;
    QueueRegistry registry;
    int ticks = 0;
    bool ok = false;

    auto listener = UdsListener::Bind(uds_path);
    if (!listener.valid()) {
        std::fprintf(stderr, "smoke: bind failed %s\n", uds_path.c_str());
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "smoke: uds=%s dataset=%s\n", uds_path.c_str(), dataset.c_str());

    std::thread server([&] {
        const int raw = listener.AcceptTimeout(5000);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        const auto ack = ServeCreateQueues(conn, registry);
        if (ack.status != AckStatus::Ok) {
            return;
        }
        SessionConfig cfg;
        cfg.population_id = ack.population_id;
        cfg.drain_timeout_ms = 5'000;
        PopulationSession session(conn, registry, std::move(cfg));
        status = session.Run();
    });

    std::thread client([&] {
        const int raw = UdsConnect(uds_path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = "smoke"});
        if (ack.status != AckStatus::Ok) {
            std::fprintf(stderr, "smoke: Create failed: %s\n", ack.message.c_str());
            return;
        }

        SpscBus out(registry.FindOut(ack.out_queue));
        SpscBus in(registry.FindIn(ack.in_queue));
        if (!out.valid() || !in.valid()) {
            std::fprintf(stderr, "smoke: missing queues (in-process only)\n");
            return;
        }

        if (!in.TrySend(BusMsgType::Open, EncodeBusOpen(MakeOpen("smoke", dataset)))) {
            return;
        }

        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        if (!RecvMessage(out, type, payload) || type != BusMsgType::OpenAck) {
            return;
        }
        BusOpenAck open_ack;
        if (!DecodeBusOpenAck(payload.data(), payload.size(), open_ack) ||
            open_ack.status != AckStatus::Ok) {
            std::fprintf(stderr, "smoke: OpenAck error: %s\n", open_ack.message.c_str());
            return;
        }

        for (;;) {
            if (!RecvMessage(out, type, payload)) {
                return;
            }
            if (type == BusMsgType::End) {
                if (!DecodeBusEnd(payload.data(), payload.size())) {
                    return;
                }
                break;
            }
            if (type != BusMsgType::Tick) {
                std::fprintf(stderr, "smoke: unexpected type %u\n",
                             static_cast<unsigned>(type));
                return;
            }
            BusTick tick;
            if (!DecodeBusTick(payload.data(), payload.size(), tick)) {
                return;
            }
            ++ticks;
            std::fprintf(stderr, "smoke: tick pop_ts_ns=%llu slots=%zu\n",
                         static_cast<unsigned long long>(tick.pop_ts_ns), tick.slots.size());
            if (!in.TrySend(BusMsgType::Advance, EncodeBusAdvance())) {
                return;
            }
        }

        if (!WaitSessionEnded(conn, ended)) {
            return;
        }
        (void)in.TrySend(BusMsgType::Close, EncodeBusClose());
        ok = true;
    });

    server.join();
    client.join();

    if (!ok || status != FinishSessionStatus::ClosedByClient || ticks < 1) {
        std::fprintf(stderr, "smoke: FAILED status=%u ticks=%d\n",
                     static_cast<unsigned>(status), ticks);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "smoke: OK ticks=%d session_ended=%s\n", ticks,
                 ended.population_id.c_str());
    return EXIT_SUCCESS;
}
