#include "transport/spsc_ring.hpp"

#include <gtest/gtest.h>

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
