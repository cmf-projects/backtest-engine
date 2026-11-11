#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/queue_registry.hpp"
#include "transport/spsc_bus.hpp"
#include "transport/uds_control.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <unistd.h>

using transport::AckStatus;
using transport::BusMsgType;
using transport::CreateQueues;
using transport::EncodeBusClose;
using transport::ErrorCode;
using transport::FinishSession;
using transport::FinishSessionStatus;
using transport::MakeSessionEndedFatal;
using transport::MakeSessionEndedOk;
using transport::QueueRegistry;
using transport::RequestCreateQueues;
using transport::ServeCreateQueues;
using transport::SessionEnded;
using transport::SpscBus;
using transport::UdsConn;
using transport::UdsConnect;
using transport::UdsListener;
using transport::WaitSessionEnded;

namespace {

std::string TempSockPath() {
    return "/tmp/cmf-uds-finish-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(::getpid()) + ".sock";
}

}  // namespace

TEST(FinishSession, CloseThenDestroy) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    FinishSessionStatus status = FinishSessionStatus::Failed;
    SessionEnded seen;

    std::thread server([&] {
        const int raw = listener.AcceptTimeout(5000);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (ServeCreateQueues(conn, registry).status != AckStatus::Ok) {
            return;
        }
        status = FinishSession(conn, registry,
                               MakeSessionEndedOk("pop-1", ErrorCode::Unspecified, "tape end"),
                               /*drain_timeout_ms=*/2'000);
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = "pop-1"});
        if (ack.status != AckStatus::Ok) {
            return;
        }
        if (!WaitSessionEnded(conn, seen)) {
            return;
        }
        SpscBus in(registry.FindIn(ack.in_queue));
        (void)in.TrySend(BusMsgType::Close, EncodeBusClose());
    });

    server.join();
    client.join();

    EXPECT_EQ(status, FinishSessionStatus::ClosedByClient);
    EXPECT_EQ(seen.population_id, "pop-1");
    EXPECT_EQ(registry.size(), 0u);
}

TEST(FinishSession, TimeoutStillDestroys) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    FinishSessionStatus status = FinishSessionStatus::Failed;

    std::thread server([&] {
        const int raw = listener.AcceptTimeout(5000);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (ServeCreateQueues(conn, registry).status != AckStatus::Ok) {
            return;
        }
        status = FinishSession(
            conn, registry,
            MakeSessionEndedFatal("pop-2", ErrorCode::Internal, "boom"),
            /*drain_timeout_ms=*/50);
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (RequestCreateQueues(conn, CreateQueues{.population_id = "pop-2"}).status !=
            AckStatus::Ok) {
            return;
        }
        SessionEnded ended;
        if (!WaitSessionEnded(conn, ended)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    });

    server.join();
    client.join();

    EXPECT_EQ(status, FinishSessionStatus::TimedOut);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(FinishSession, CoalescedCreateAckAndSessionEnded) {
    // Sticky buffer: SessionEnded may land before CreateAck is fully read.
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    FinishSessionStatus status = FinishSessionStatus::Failed;
    SessionEnded seen;

    std::thread server([&] {
        const int raw = listener.AcceptTimeout(5000);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (ServeCreateQueues(conn, registry).status != AckStatus::Ok) {
            return;
        }
        status = FinishSession(conn, registry,
                               MakeSessionEndedOk("pop-3", ErrorCode::ClientClose, "done"),
                               /*drain_timeout_ms=*/2'000);
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = "pop-3"});
        if (ack.status != AckStatus::Ok) {
            return;
        }
        if (!WaitSessionEnded(conn, seen)) {
            return;
        }
        SpscBus in(registry.FindIn(ack.in_queue));
        (void)in.TrySend(BusMsgType::Close, EncodeBusClose());
    });

    server.join();
    client.join();

    EXPECT_EQ(status, FinishSessionStatus::ClosedByClient);
    EXPECT_EQ(seen.population_id, "pop-3");
    EXPECT_EQ(registry.size(), 0u);
}

TEST(FinishSession, AlreadySeenCloseSkipsWait) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    FinishSessionStatus status = FinishSessionStatus::Failed;
    SessionEnded seen;

    std::thread server([&] {
        const int raw = listener.AcceptTimeout(5000);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (ServeCreateQueues(conn, registry).status != AckStatus::Ok) {
            return;
        }
        status = FinishSession(conn, registry,
                               MakeSessionEndedOk("pop-4", ErrorCode::ClientClose, "early close"),
                               /*drain_timeout_ms=*/2'000,
                               /*close_already_seen=*/true);
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        if (RequestCreateQueues(conn, CreateQueues{.population_id = "pop-4"}).status !=
            AckStatus::Ok) {
            return;
        }
        (void)WaitSessionEnded(conn, seen);
    });

    server.join();
    client.join();

    EXPECT_EQ(status, FinishSessionStatus::ClosedByClient);
    EXPECT_EQ(seen.population_id, "pop-4");
    EXPECT_EQ(registry.size(), 0u);
}
