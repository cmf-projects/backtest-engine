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
        const int conn = listener.Accept();
        if (conn < 0) {
            return;
        }
        (void)ServeCreateQueues(conn, registry);
        ::close(conn);
    });

    std::thread client([&] {
        const int conn = UdsConnect(path);
        if (conn < 0) {
            return;
        }
        client_ack = RequestCreateQueues(conn, req);
        ::close(conn);
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

TEST(UdsControl, EmptyPopulationIdRejected) {
    const std::string path = TempSockPath();
    auto listener = UdsListener::Bind(path);
    QueueRegistry registry;

    transport::CreateAck client_ack;
    std::thread server([&] {
        const int conn = listener.Accept();
        if (conn < 0) {
            return;
        }
        (void)ServeCreateQueues(conn, registry);
        ::close(conn);
    });

    std::thread client([&] {
        const int conn = UdsConnect(path);
        if (conn < 0) {
            return;
        }
        client_ack = RequestCreateQueues(conn, CreateQueues{.population_id = ""});
        ::close(conn);
    });

    server.join();
    client.join();

    EXPECT_EQ(client_ack.status, AckStatus::Error);
    EXPECT_EQ(client_ack.reason_code, ErrorCode::CreateFailed);
    EXPECT_EQ(registry.size(), 0u);
}
