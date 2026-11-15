#include "transport/shm_queue.hpp"

#include <gtest/gtest.h>

#include <string>
#include <sys/wait.h>
#include <unistd.h>

using transport::AttachShmQueue;
using transport::CreateShmQueue;
using transport::IsValidPopulationId;
using transport::MakeUniqueQueueNames;
using transport::QueueMessage;
using transport::ReapStaleShmQueues;

TEST(ShmQueue, PopulationIdCharset) {
    EXPECT_TRUE(IsValidPopulationId("pop-1"));
    EXPECT_TRUE(IsValidPopulationId("A_b"));
    EXPECT_FALSE(IsValidPopulationId(""));
    EXPECT_FALSE(IsValidPopulationId("a/b"));
    EXPECT_FALSE(IsValidPopulationId("pop:1"));
    EXPECT_FALSE(IsValidPopulationId("a.b"));
}

TEST(ShmQueue, UniqueNamesDoNotCollide) {
    const auto a = MakeUniqueQueueNames();
    const auto b = MakeUniqueQueueNames();
    EXPECT_NE(a.out_name, b.out_name);
    EXPECT_NE(a.in_name, b.in_name);
    EXPECT_EQ(a.out_name.find("/cmf-"), 0u);
    EXPECT_NE(a.out_name.find("-out"), std::string::npos);
}

TEST(ShmQueue, CreateAttachPushPop) {
    const auto names = MakeUniqueQueueNames();
    auto owner = CreateShmQueue(names.out_name);
    ASSERT_TRUE(owner.has_value());
    ASSERT_TRUE(owner->valid());
    ASSERT_TRUE(owner->owner());

    auto peer = AttachShmQueue(names.out_name);
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

    EXPECT_FALSE(AttachShmQueue(names.out_name).has_value());
}

TEST(ShmQueue, ReapRemovesDeadPidOnly) {
    const auto live = MakeUniqueQueueNames();
    auto owner = CreateShmQueue(live.out_name);
    ASSERT_TRUE(owner.has_value());

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        const std::string name = "/cmf-" + std::to_string(::getpid()) + "-7-out";
        auto stale = CreateShmQueue(name);
        // _exit skips destructors, so the shm object stays behind.
        _exit(stale ? 0 : 2);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    const std::string stale = "/cmf-" + std::to_string(child) + "-7-out";
    ASSERT_TRUE(AttachShmQueue(stale).has_value());

    ReapStaleShmQueues(/*include_self=*/false);

    EXPECT_TRUE(AttachShmQueue(live.out_name).has_value());
    EXPECT_FALSE(AttachShmQueue(stale).has_value());
    owner->Reset();
}
