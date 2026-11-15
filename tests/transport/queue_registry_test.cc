#include "transport/queue_registry.hpp"
#include "transport/shm_queue.hpp"

#include <gtest/gtest.h>

#include <string>

using transport::AckStatus;
using transport::AttachShmQueue;
using transport::ErrorCode;
using transport::QueueMessage;
using transport::QueueRegistry;

TEST(QueueRegistry, CreateGivesLiveRings) {
    QueueRegistry reg;
    auto ack = reg.Create("pop-1");
    ASSERT_EQ(ack.status, AckStatus::Ok);
    EXPECT_NE(ack.out_queue.find("/cmf-"), std::string::npos);
    EXPECT_NE(ack.out_queue.find("-out"), std::string::npos);
    EXPECT_NE(ack.in_queue.find("-in"), std::string::npos);
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

TEST(QueueRegistry, SameIdInTwoRegistriesAreDistinct) {
    QueueRegistry a;
    QueueRegistry b;
    const auto aa = a.Create("same");
    const auto bb = b.Create("same");
    ASSERT_EQ(aa.status, AckStatus::Ok);
    ASSERT_EQ(bb.status, AckStatus::Ok);
    EXPECT_NE(aa.out_queue, bb.out_queue);

    QueueMessage msg;
    msg.size = 1;
    msg.bytes[0] = 0x42;
    ASSERT_TRUE(a.FindOut(aa.out_queue)->TryPush(msg));

    auto attach_a = AttachShmQueue(aa.out_queue);
    auto attach_b = AttachShmQueue(bb.out_queue);
    ASSERT_TRUE(attach_a.has_value());
    ASSERT_TRUE(attach_b.has_value());

    QueueMessage got;
    EXPECT_TRUE(attach_a->get()->TryPop(got));
    EXPECT_EQ(got.bytes[0], 0x42);
    EXPECT_FALSE(attach_b->get()->TryPop(got));

    ASSERT_TRUE(a.Destroy("same"));
    EXPECT_FALSE(AttachShmQueue(aa.out_queue).has_value());
    EXPECT_TRUE(AttachShmQueue(bb.out_queue).has_value());
}

TEST(QueueRegistry, RejectsUnsafePopulationId) {
    QueueRegistry reg;
    const auto ack = reg.Create("a/b");
    EXPECT_EQ(ack.status, AckStatus::Error);
    EXPECT_EQ(ack.reason_code, ErrorCode::CreateFailed);
    EXPECT_EQ(reg.size(), 0u);
}
