#include "transport/queue_registry.hpp"

#include <gtest/gtest.h>

using transport::AckStatus;
using transport::ErrorCode;
using transport::QueueMessage;
using transport::QueueRegistry;

TEST(QueueRegistry, CreateGivesLiveRings) {
    QueueRegistry reg;
    auto ack = reg.Create("pop-1");
    ASSERT_EQ(ack.status, AckStatus::Ok);
    EXPECT_EQ(ack.out_queue, "out:pop-1");
    EXPECT_EQ(ack.in_queue, "in:pop-1");
    EXPECT_EQ(reg.size(), 1u);

    auto* out = reg.FindOut(ack.out_queue);
    auto* in = reg.FindIn(ack.in_queue);
    ASSERT_NE(out, nullptr);
    ASSERT_NE(in, nullptr);

    QueueMessage msg;
    msg.size = 3;
    msg.bytes[0] = 'a';
    msg.bytes[1] = 'b';
    msg.bytes[2] = 'c';
    EXPECT_TRUE(out->TryPush(msg));

    QueueMessage got;
    EXPECT_TRUE(out->TryPop(got));
    EXPECT_EQ(got.size, 3u);
    EXPECT_EQ(got.bytes[0], 'a');
}

TEST(QueueRegistry, DuplicateRejected) {
    QueueRegistry reg;
    ASSERT_EQ(reg.Create("pop-1").status, AckStatus::Ok);
    auto ack = reg.Create("pop-1");
    EXPECT_EQ(ack.status, AckStatus::Error);
    EXPECT_EQ(ack.reason_code, ErrorCode::CreateFailed);
    EXPECT_EQ(reg.size(), 1u);
}

TEST(QueueRegistry, DestroyRemoves) {
    QueueRegistry reg;
    auto ack = reg.Create("pop-1");
    ASSERT_TRUE(reg.Destroy("pop-1"));
    EXPECT_EQ(reg.size(), 0u);
    EXPECT_EQ(reg.FindOut(ack.out_queue), nullptr);
    EXPECT_FALSE(reg.Destroy("pop-1"));
}
