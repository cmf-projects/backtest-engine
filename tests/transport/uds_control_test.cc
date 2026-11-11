#include "transport/queue_registry.hpp"
#include "transport/uds_control.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <unistd.h>

using transport::AckStatus;
using transport::CreateQueues;
using transport::ErrorCode;
using transport::QueueMessage;
using transport::QueueRegistry;
using transport::RequestCreateQueues;
using transport::ServeCreateQueues;
using transport::UdsConn;
using transport::UdsConnect;
using transport::UdsListener;

namespace {

std::string TempSockPath() {
    return "/tmp/cmf-uds-test-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(::getpid()) + ".sock";
}

}  // namespace

TEST(UdsControl, CreateQueuesRegistersRings) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    ASSERT_TRUE(listener.valid());

    QueueRegistry registry;
    CreateQueues req{.population_id = "pop-42"};
    transport::CreateAck client_ack;

    std::thread server([&] {
        const int raw = listener.AcceptTimeout(5000);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        (void)ServeCreateQueues(conn, registry);
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        client_ack = RequestCreateQueues(conn, req);
    });

    server.join();
    client.join();

    ASSERT_EQ(client_ack.status, AckStatus::Ok);
    EXPECT_EQ(client_ack.population_id, "pop-42");
    EXPECT_EQ(registry.size(), 1u);

    auto* out = registry.FindOut(client_ack.out_queue);
    ASSERT_NE(out, nullptr);
    QueueMessage msg;
    msg.size = 1;
    msg.bytes[0] = 7;
    EXPECT_TRUE(out->TryPush(msg));
    QueueMessage got;
    EXPECT_TRUE(out->TryPop(got));
    EXPECT_EQ(got.bytes[0], 7);
}

TEST(UdsControl, AcceptTimeoutWhenNoClient) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    ASSERT_TRUE(listener.valid());

    const auto t0 = std::chrono::steady_clock::now();
    const int raw = listener.AcceptTimeout(50);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(raw, -2);
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));
}

TEST(UdsControl, EmptyPopulationIdRejected) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    transport::CreateAck client_ack;
    std::thread server([&] {
        const int raw = listener.AcceptTimeout(5000);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        (void)ServeCreateQueues(conn, registry);
    });

    std::thread client([&] {
        const int raw = UdsConnect(path);
        if (raw < 0) {
            return;
        }
        UdsConn conn(raw);
        client_ack = RequestCreateQueues(conn, CreateQueues{.population_id = ""});
    });

    server.join();
    client.join();

    EXPECT_EQ(client_ack.status, AckStatus::Error);
    EXPECT_EQ(client_ack.reason_code, ErrorCode::CreateFailed);
    EXPECT_EQ(registry.size(), 0u);
}
