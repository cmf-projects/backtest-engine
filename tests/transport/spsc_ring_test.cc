#include "transport/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using transport::SpscRing;

TEST(SpscRing, PushPopFifo) {
    SpscRing<std::uint64_t, 8> q;
    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.TryPush(10));
    EXPECT_TRUE(q.TryPush(20));
    EXPECT_FALSE(q.empty());

    std::uint64_t v = 0;
    EXPECT_TRUE(q.TryPop(v));
    EXPECT_EQ(v, 10u);
    EXPECT_TRUE(q.TryPop(v));
    EXPECT_EQ(v, 20u);
    EXPECT_FALSE(q.TryPop(v));
    EXPECT_TRUE(q.empty());
}

TEST(SpscRing, FullRejectsPush) {
    // Usable slots = Capacity - 1.
    SpscRing<int, 4> q;
    EXPECT_TRUE(q.TryPush(1));
    EXPECT_TRUE(q.TryPush(2));
    EXPECT_TRUE(q.TryPush(3));
    EXPECT_FALSE(q.TryPush(4));

    int v = 0;
    EXPECT_TRUE(q.TryPop(v));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.TryPush(4));
    EXPECT_FALSE(q.TryPush(5));
}

TEST(SpscRing, TwoThreadsTransfer) {
    constexpr std::size_t n = 10'000;
    SpscRing<std::uint64_t, 1024> q;
    std::vector<std::uint64_t> got;
    got.reserve(n);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < n;) {
            if (q.TryPush(i)) {
                ++i;
            }
        }
    });

    std::thread consumer([&] {
        while (got.size() < n) {
            std::uint64_t v = 0;
            if (q.TryPop(v)) {
                got.push_back(v);
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(got.size(), n);
    for (std::uint64_t i = 0; i < n; ++i) {
        EXPECT_EQ(got[i], i);
    }
}

TEST(SpscRing, WaitWakesOnPush) {
    SpscRing<int, 8> q;
    int got = -1;
    std::thread consumer([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            int v = 0;
            if (q.TryPop(v)) {
                got = v;
                return;
            }
            q.Wait(deadline);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ASSERT_TRUE(q.TryPush(7));
    consumer.join();
    EXPECT_EQ(got, 7);
}

TEST(SpscRing, WaitTimesOutWhenIdle) {
    SpscRing<int, 8> q;
    const auto started = std::chrono::steady_clock::now();
    q.Wait(started + std::chrono::milliseconds(50));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, std::chrono::milliseconds(40));
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));
}
