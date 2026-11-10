#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/population_session.hpp"
#include "transport/queue_registry.hpp"
#include "transport/spsc_bus.hpp"
#include "transport/uds_control.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using population::Level;
using population::SnapshotEvent;
using population::test::FakeTape;
using transport::AckStatus;
using transport::BusAssetRole;
using transport::BusComponent;
using transport::BusMsgType;
using transport::BusOpen;
using transport::BusOpenAck;
using transport::BusOrder;
using transport::BusOrderBatch;
using transport::BusOrdersAck;
using transport::BusSide;
using transport::BusSlotConfig;
using transport::BusTick;
using transport::BusTimeInForce;
using transport::CreateQueues;
using transport::DecodeBusEnd;
using transport::DecodeBusError;
using transport::DecodeBusOpenAck;
using transport::DecodeBusOrdersAck;
using transport::DecodeBusTick;
using transport::EncodeBusAdvance;
using transport::EncodeBusClose;
using transport::EncodeBusOpen;
using transport::EncodeBusOrderBatch;
using transport::ErrorCode;
using transport::FinishSessionStatus;
using transport::PopulationSession;
using transport::QueueRegistry;
using transport::RequestCreateQueues;
using transport::ServeCreateQueues;
using transport::SessionConfig;
using transport::SessionEnded;
using transport::SessionEndStatus;
using transport::SpscBus;
using transport::UdsConn;
using transport::UdsConnect;
using transport::UdsListener;
using transport::WaitSessionEnded;
using transport::BusRecvResult;

namespace {

std::string TempSockPath() {
    return "/tmp/cmf-uds-session-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(::getpid()) + ".sock";
}

bool RecvMessage(SpscBus& bus, BusMsgType& type, std::vector<std::uint8_t>& payload,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
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

SnapshotEvent Ev(std::uint64_t seq, std::uint64_t ts_ns, std::vector<Level> bids,
                 std::vector<Level> asks) {
    SnapshotEvent e;
    e.seq = seq;
    e.ts_ns = ts_ns;
    e.symbol = "SBER";
    e.bids = std::move(bids);
    e.asks = std::move(asks);
    return e;
}

BusOpen MakeTestOpen(std::string population_id, std::uint32_t n_slots) {
    BusOpen open;
    open.population_id = std::move(population_id);
    open.components.push_back(BusComponent{
        .component_id = "SPOT",
        .role = BusAssetRole::Spot,
        .dataset_path = "/tmp/stub.jsonl",
        .class_code = "TQBR",
        .sec_code = "SBER",
        .tick_size = "0.01",
    });
    for (std::uint32_t i = 0; i < n_slots; ++i) {
        open.slots.push_back(BusSlotConfig{
            .slot_id = i + 1,
            .md_lag_ns = 0,
            .delta_ns = 1,
            .impact_lambda = "0",
        });
    }
    return open;
}

SessionConfig MakeCfg(std::string population_id, std::vector<SnapshotEvent> events) {
    SessionConfig cfg;
    cfg.population_id = std::move(population_id);
    cfg.drain_timeout_ms = 2'000;
    cfg.make_tape = [events = std::move(events)](const BusComponent&) mutable {
        return std::make_unique<FakeTape>(std::move(events));
    };
    return cfg;
}

}  // namespace

TEST(PopulationSession, OpenTickAdvanceEndWithPopulation) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    FinishSessionStatus status = FinishSessionStatus::Failed;
    SessionEnded ended;

    auto events = std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 200, {{100.0, 8}}, {{101.0, 5}}),
    };

    std::thread server([&] {
        const int raw = listener.Accept();
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (ServeCreateQueues(conn, registry).status != AckStatus::Ok) {
            return;
        }
        PopulationSession session(conn, registry, MakeCfg("pop-n", std::move(events)));
        status = session.Run();
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = "pop-n"});
        if (ack.status != AckStatus::Ok) {
            return;
        }

        SpscBus out(registry.FindOut(ack.out_queue));
        SpscBus in(registry.FindIn(ack.in_queue));

        if (!in.TrySend(BusMsgType::Open, EncodeBusOpen(MakeTestOpen("pop-n", 1)))) {
            return;
        }

        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        if (!RecvMessage(out, type, payload) || type != BusMsgType::OpenAck) {
            return;
        }
        BusOpenAck open_ack;
        if (!DecodeBusOpenAck(payload.data(), payload.size(), open_ack) ||
            open_ack.status != AckStatus::Ok || open_ack.n_slots != 1u) {
            return;
        }

        if (!RecvMessage(out, type, payload) || type != BusMsgType::Tick) {
            return;
        }
        BusTick tick;
        if (!DecodeBusTick(payload.data(), payload.size(), tick) || tick.pop_ts_ns != 100u ||
            tick.slots.size() != 1u || tick.slots[0].books.empty()) {
            return;
        }
        EXPECT_EQ(tick.slots[0].books[0].component_id, "SPOT");
        EXPECT_FALSE(tick.slots[0].books[0].bids.empty());

        // Advance to second event → another Tick.
        if (!in.TrySend(BusMsgType::Advance, EncodeBusAdvance())) {
            return;
        }
        if (!RecvMessage(out, type, payload) || type != BusMsgType::Tick) {
            return;
        }
        if (!DecodeBusTick(payload.data(), payload.size(), tick) || tick.pop_ts_ns != 200u) {
            return;
        }

        // Advance past EOF → End.
        if (!in.TrySend(BusMsgType::Advance, EncodeBusAdvance())) {
            return;
        }
        if (!RecvMessage(out, type, payload) || type != BusMsgType::End ||
            !DecodeBusEnd(payload.data(), payload.size())) {
            return;
        }

        if (!WaitSessionEnded(conn, ended)) {
            return;
        }
        (void)in.TrySend(BusMsgType::Close, EncodeBusClose());
    });

    server.join();
    client.join();

    EXPECT_EQ(status, FinishSessionStatus::ClosedByClient);
    EXPECT_EQ(ended.status, SessionEndStatus::Ok);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(PopulationSession, SubmitOrderThenAdvance) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    FinishSessionStatus status = FinishSessionStatus::Failed;
    SessionEnded ended;

    auto events = std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 200, {{100.0, 10}}, {{101.0, 5}}),
    };

    std::thread server([&] {
        const int raw = listener.Accept();
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (ServeCreateQueues(conn, registry).status != AckStatus::Ok) {
            return;
        }
        PopulationSession session(conn, registry, MakeCfg("pop-o", std::move(events)));
        status = session.Run();
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = "pop-o"});
        if (ack.status != AckStatus::Ok) {
            return;
        }

        SpscBus out(registry.FindOut(ack.out_queue));
        SpscBus in(registry.FindIn(ack.in_queue));

        if (!in.TrySend(BusMsgType::Open, EncodeBusOpen(MakeTestOpen("pop-o", 1)))) {
            return;
        }

        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        if (!RecvMessage(out, type, payload) || type != BusMsgType::OpenAck) {
            return;
        }
        if (!RecvMessage(out, type, payload) || type != BusMsgType::Tick) {
            return;
        }

        BusOrderBatch batch;
        batch.orders.push_back(BusOrder{
            .slot_id = 1,
            .client_order_id = 7,
            .component_id = "SPOT",
            .side = BusSide::Buy,
            .price = "100.00",
            .qty = 1,
            .tif = BusTimeInForce::Day,
        });
        if (!in.TrySend(BusMsgType::OrderBatch, EncodeBusOrderBatch(batch))) {
            return;
        }
        if (!RecvMessage(out, type, payload) || type != BusMsgType::OrdersAck) {
            return;
        }
        BusOrdersAck orders_ack;
        if (!DecodeBusOrdersAck(payload.data(), payload.size(), orders_ack) ||
            orders_ack.status != AckStatus::Ok) {
            return;
        }

        if (!in.TrySend(BusMsgType::Advance, EncodeBusAdvance())) {
            return;
        }
        if (!RecvMessage(out, type, payload) || type != BusMsgType::Tick) {
            return;
        }

        if (!in.TrySend(BusMsgType::Advance, EncodeBusAdvance())) {
            return;
        }
        if (!RecvMessage(out, type, payload) || type != BusMsgType::End) {
            return;
        }

        if (!WaitSessionEnded(conn, ended)) {
            return;
        }
        (void)in.TrySend(BusMsgType::Close, EncodeBusClose());
    });

    server.join();
    client.join();

    EXPECT_EQ(status, FinishSessionStatus::ClosedByClient);
    EXPECT_EQ(ended.status, SessionEndStatus::Ok);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(PopulationSession, ClientCloseAfterTick) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    FinishSessionStatus status = FinishSessionStatus::Failed;
    SessionEnded ended;

    auto events = std::vector<SnapshotEvent>{
        Ev(1, 100, {{100.0, 10}}, {{101.0, 5}}),
        Ev(2, 200, {{100.0, 8}}, {{101.0, 5}}),
    };

    std::thread server([&] {
        const int raw = listener.Accept();
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (ServeCreateQueues(conn, registry).status != AckStatus::Ok) {
            return;
        }
        PopulationSession session(conn, registry, MakeCfg("pop-c", std::move(events)));
        status = session.Run();
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = "pop-c"});
        if (ack.status != AckStatus::Ok) {
            return;
        }

        SpscBus out(registry.FindOut(ack.out_queue));
        SpscBus in(registry.FindIn(ack.in_queue));

        if (!in.TrySend(BusMsgType::Open, EncodeBusOpen(MakeTestOpen("pop-c", 1)))) {
            return;
        }

        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        if (!RecvMessage(out, type, payload) || type != BusMsgType::OpenAck) {
            return;
        }
        if (!RecvMessage(out, type, payload) || type != BusMsgType::Tick) {
            return;
        }

        if (!in.TrySend(BusMsgType::Close, EncodeBusClose())) {
            return;
        }
        (void)WaitSessionEnded(conn, ended);
    });

    server.join();
    client.join();

    EXPECT_EQ(status, FinishSessionStatus::ClosedByClient);
    EXPECT_EQ(ended.status, SessionEndStatus::Ok);
    EXPECT_EQ(ended.reason_code, ErrorCode::ClientClose);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(PopulationSession, UnexpectedMessageIsFatal) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    FinishSessionStatus status = FinishSessionStatus::Failed;
    SessionEnded ended;

    std::thread server([&] {
        const int raw = listener.Accept();
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (ServeCreateQueues(conn, registry).status != AckStatus::Ok) {
            return;
        }
        // Tape unused — fail before Open completes.
        PopulationSession session(conn, registry,
                                  MakeCfg("pop-f", {Ev(1, 1, {{1.0, 1}}, {{2.0, 1}})}));
        status = session.Run();
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = "pop-f"});
        if (ack.status != AckStatus::Ok) {
            return;
        }

        SpscBus out(registry.FindOut(ack.out_queue));
        SpscBus in(registry.FindIn(ack.in_queue));

        if (!in.TrySend(BusMsgType::Advance, EncodeBusAdvance())) {
            return;
        }

        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        (void)RecvMessage(out, type, payload);
        if (type == BusMsgType::Error) {
            transport::BusError err;
            (void)DecodeBusError(payload.data(), payload.size(), err);
        }

        if (!WaitSessionEnded(conn, ended)) {
            return;
        }
        (void)in.TrySend(BusMsgType::Close, EncodeBusClose());
    });

    server.join();
    client.join();

    EXPECT_EQ(status, FinishSessionStatus::ClosedByClient);
    EXPECT_EQ(ended.status, SessionEndStatus::Fatal);
    EXPECT_EQ(registry.size(), 0u);
}
