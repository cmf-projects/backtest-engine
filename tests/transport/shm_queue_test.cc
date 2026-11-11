#include "transport/shm_queue.hpp"

#include <gtest/gtest.h>

#include <string>

using transport::AttachShmQueue;
using transport::CreateShmQueue;
using transport::MakeInQueueName;
using transport::MakeOutQueueName;
using transport::QueueMessage;
using transport::SanitizeShmToken;

TEST(ShmQueue, NameHelpers) {
    EXPECT_EQ(SanitizeShmToken("pop:1"), "pop_1");
    EXPECT_EQ(MakeOutQueueName("take"), "/cmf-take-out");
    EXPECT_EQ(MakeInQueueName("take"), "/cmf-take-in");
}

TEST(ShmQueue, CreateAttachPushPop) {
    const std::string name = "/cmf-test-shm-roundtrip";
    auto owner = CreateShmQueue(name);
    ASSERT_TRUE(owner.has_value());
    ASSERT_TRUE(owner->valid());
    ASSERT_TRUE(owner->owner());

    auto peer = AttachShmQueue(name);
    ASSERT_TRUE(peer.has_value());
    ASSERT_TRUE(peer->valid());
    ASSERT_FALSE(peer->owner());

    QueueMessage msg;
    msg.size = 2;
    msg.bytes[0] = 0x11;
    msg.bytes[1] = 0x22;
    EXPECT_TRUE(owner->get()->TryPush(msg));

    QueueMessage got;
    EXPECT_TRUE(peer->get()->TryPop(got));
    EXPECT_EQ(got.size, 2u);
    EXPECT_EQ(got.bytes[0], 0x11);
    EXPECT_EQ(got.bytes[1], 0x22);

    peer->Reset();
    owner->Reset();

    EXPECT_FALSE(AttachShmQueue(name).has_value());
}
