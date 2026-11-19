#include "orders.hpp"

#include <gtest/gtest.h>

namespace {

client::PendingOrder Order(const char* component) {
    client::PendingOrder order;
    order.submit_pop_ts_ns = 1;
    order.component_id = component;
    return order;
}

}  // namespace

TEST(DensePending, EraseClearsTheSlotAndReusesIt) {
    client::DensePending pending;
    for (std::uint64_t oid = 1; oid <= 100'000; ++oid) {
        pending.Put(oid, Order("SPOT"));
        ASSERT_NE(pending.Find(oid), nullptr);
        pending.Erase(oid);
        EXPECT_EQ(pending.Find(oid), nullptr);
    }
    EXPECT_EQ(pending.live_count(), 0u);
    EXPECT_EQ(pending.slot_count(), 1u);
}

TEST(DensePending, LiveOrdersStayWhileAFinishedIdIsReused) {
    client::DensePending pending;
    pending.Put(10, Order("SPOT"));
    pending.Put(11, Order("FUT"));
    pending.Put(12, Order("OPT"));
    pending.Erase(11);

    pending.Put(1'000'000, Order("FUT"));

    EXPECT_EQ(pending.slot_count(), 3u);
    EXPECT_EQ(pending.live_count(), 3u);
    EXPECT_EQ(pending.Find(11), nullptr);
    ASSERT_NE(pending.Find(10), nullptr);
    EXPECT_EQ(pending.Find(10)->component_id, "SPOT");
    ASSERT_NE(pending.Find(1'000'000), nullptr);
    EXPECT_EQ(pending.Find(1'000'000)->component_id, "FUT");
    ASSERT_NE(pending.Find(12), nullptr);
    EXPECT_EQ(pending.Find(12)->component_id, "OPT");
}

TEST(DensePending, PutSameIdOverwritesAndDoesNotAddASlot) {
    client::DensePending pending;
    pending.Put(7, Order("SPOT"));
    pending.Put(7, Order("FUT"));
    EXPECT_EQ(pending.live_count(), 1u);
    EXPECT_EQ(pending.slot_count(), 1u);
    ASSERT_NE(pending.Find(7), nullptr);
    EXPECT_EQ(pending.Find(7)->component_id, "FUT");
}
