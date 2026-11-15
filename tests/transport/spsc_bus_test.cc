#include "transport/bus_framing.hpp"
#include "transport/queue_registry.hpp"
#include "transport/spsc_bus.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

using transport::AckStatus;
using transport::BusAssetRole;
using transport::BusComponent;
using transport::BusError;
using transport::BusMsgType;
using transport::BusOpen;
using transport::BusOpenAck;
using transport::BusSlotConfig;
using transport::BusTick;
using transport::DecodeBusAdvance;
using transport::DecodeBusClose;
using transport::DecodeBusEnd;
using transport::DecodeBusError;
using transport::DecodeBusOpen;
using transport::DecodeBusOpenAck;
using transport::DecodeBusTick;
using transport::EncodeBusAdvance;
using transport::EncodeBusClose;
using transport::EncodeBusEnd;
using transport::EncodeBusError;
using transport::EncodeBusOpen;
using transport::EncodeBusOpenAck;
using transport::EncodeBusTick;
using transport::ErrorCode;
using transport::MakeBusOpenOk;
using transport::QueueRegistry;
using transport::BusRecvResult;
using transport::SpscBus;
using transport::kMaxBusChunkBytes;
using transport::kMaxLogicalBusBytes;

TEST(SpscBus, OpenAckTickAdvanceEndRoundTrip) {
    QueueRegistry reg;
    auto ack = reg.Create("pop-1");
    ASSERT_EQ(ack.status, AckStatus::Ok);

    SpscBus out(reg.FindOut(ack.out_queue));
    SpscBus in(reg.FindIn(ack.in_queue));
    ASSERT_TRUE(out.valid());
    ASSERT_TRUE(in.valid());

    BusOpen open_msg{.population_id = "pop-1"};
    open_msg.components.push_back(BusComponent{
        .component_id = "SPOT",
        .role = BusAssetRole::Spot,
        .dataset_path = "x.jsonl",
        .tick_size = "0.01",
    });
    for (std::uint32_t i = 0; i < 4; ++i) {
        open_msg.slots.push_back(BusSlotConfig{
            .slot_id = i + 1, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = "0"});
    }
    ASSERT_TRUE(in.TrySend(BusMsgType::Open, EncodeBusOpen(open_msg)));
    {
        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        ASSERT_EQ(in.TryRecv(type, payload), BusRecvResult::Message);
        EXPECT_EQ(type, BusMsgType::Open);
        BusOpen open;
        ASSERT_TRUE(DecodeBusOpen(payload.data(), payload.size(), open));
        EXPECT_EQ(open.population_id, "pop-1");
        EXPECT_EQ(open.slots.size(), 4u);
    }

    ASSERT_TRUE(out.TrySend(BusMsgType::OpenAck, EncodeBusOpenAck(MakeBusOpenOk("pop-1", 4))));
    BusTick tick_msg{.pop_ts_ns = 100};
    tick_msg.slots.push_back({.slot_id = 1});
    ASSERT_TRUE(out.TrySend(BusMsgType::Tick, EncodeBusTick(tick_msg)));

    {
        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        ASSERT_EQ(out.TryRecv(type, payload), BusRecvResult::Message);
        EXPECT_EQ(type, BusMsgType::OpenAck);
        BusOpenAck open_ack;
        ASSERT_TRUE(DecodeBusOpenAck(payload.data(), payload.size(), open_ack));
        EXPECT_EQ(open_ack.status, AckStatus::Ok);
        EXPECT_EQ(open_ack.n_slots, 4u);

        ASSERT_EQ(out.TryRecv(type, payload), BusRecvResult::Message);
        EXPECT_EQ(type, BusMsgType::Tick);
        BusTick tick;
        ASSERT_TRUE(DecodeBusTick(payload.data(), payload.size(), tick));
        EXPECT_EQ(tick.pop_ts_ns, 100u);
        ASSERT_EQ(tick.slots.size(), 1u);
        EXPECT_EQ(tick.slots[0].slot_id, 1u);
    }

    ASSERT_TRUE(in.TrySend(BusMsgType::Advance, EncodeBusAdvance()));
    {
        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        ASSERT_EQ(in.TryRecv(type, payload), BusRecvResult::Message);
        EXPECT_EQ(type, BusMsgType::Advance);
        EXPECT_TRUE(DecodeBusAdvance(payload.data(), payload.size()));
    }

    ASSERT_TRUE(out.TrySend(BusMsgType::End, EncodeBusEnd()));
    {
        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        ASSERT_EQ(out.TryRecv(type, payload), BusRecvResult::Message);
        EXPECT_EQ(type, BusMsgType::End);
        EXPECT_TRUE(DecodeBusEnd(payload.data(), payload.size()));
    }
}

TEST(SpscBus, ErrorAndClose) {
    QueueRegistry reg;
    auto created = reg.Create("pop-2");
    SpscBus out(reg.FindOut(created.out_queue));
    SpscBus in(reg.FindIn(created.in_queue));

    BusError err{.code = ErrorCode::Internal, .message = "bad phase"};
    ASSERT_TRUE(out.TrySend(BusMsgType::Error, EncodeBusError(err)));

    BusMsgType type{};
    std::vector<std::uint8_t> payload;
    ASSERT_EQ(out.TryRecv(type, payload), BusRecvResult::Message);
    EXPECT_EQ(type, BusMsgType::Error);
    BusError got;
    ASSERT_TRUE(DecodeBusError(payload.data(), payload.size(), got));
    EXPECT_EQ(got.code, ErrorCode::Internal);

    ASSERT_TRUE(in.TrySend(BusMsgType::Close, EncodeBusClose()));
    ASSERT_EQ(in.TryRecv(type, payload), BusRecvResult::Message);
    EXPECT_EQ(type, BusMsgType::Close);
    EXPECT_TRUE(DecodeBusClose(payload.data(), payload.size()));
}

TEST(SpscBus, MultiSlotRoundTrip) {
    QueueRegistry reg;
    auto created = reg.Create("pop-3");
    SpscBus out(reg.FindOut(created.out_queue));

    // Bigger than one chunk → must span slots.
    std::vector<std::uint8_t> big(kMaxBusChunkBytes * 3 + 100);
    std::iota(big.begin(), big.end(), std::uint8_t{0});

    ASSERT_TRUE(out.TrySend(BusMsgType::Tick, big));

    BusMsgType type{};
    std::vector<std::uint8_t> payload;
    ASSERT_EQ(out.TryRecv(type, payload), BusRecvResult::Message);
    EXPECT_EQ(type, BusMsgType::Tick);
    EXPECT_EQ(payload, big);
}

TEST(SpscBus, TruncatedLastIsProtocolError) {
    QueueRegistry reg;
    auto created = reg.Create("pop-5");
    auto* q = reg.FindOut(created.out_queue);
    ASSERT_NE(q, nullptr);

    // Hand-craft First|Last that claims total=10 but carries 0 payload bytes.
    transport::QueueMessage slot{};
    slot.bytes[0] = static_cast<std::uint8_t>(BusMsgType::Tick);
    slot.bytes[4] = static_cast<std::uint8_t>(transport::kBusFlagFirst | transport::kBusFlagLast);
    slot.bytes[8] = 10;  // total LE
    slot.size = transport::kBusSlotHeaderBytes;
    ASSERT_TRUE(q->TryPush(slot));

    SpscBus out(q);
    BusMsgType type{};
    std::vector<std::uint8_t> payload;
    EXPECT_EQ(out.TryRecv(type, payload), BusRecvResult::ProtocolError);
}

TEST(SpscBus, SoftCapRejected) {
    QueueRegistry reg;
    auto created = reg.Create("pop-4");
    SpscBus out(reg.FindOut(created.out_queue));

    std::vector<std::uint8_t> huge(kMaxLogicalBusBytes + 1, 0xab);
    EXPECT_FALSE(out.TrySend(BusMsgType::Tick, huge));
}

TEST(SpscBus, SendLargerThanRingWaitsForReader) {
    QueueRegistry reg;
    auto created = reg.Create("pop-wide");
    auto* queue = reg.FindOut(created.out_queue);
    ASSERT_NE(queue, nullptr);
    SpscBus sender(queue);
    SpscBus reader(queue);

    // One byte past an empty ring: the send has to wait for a pop.
    const std::size_t nbytes =
        transport::kMaxBusChunkBytes * (transport::kQueueCapacity - 1) + 1;
    std::vector<std::uint8_t> big(nbytes, 0xcd);

    std::vector<std::uint8_t> got;
    BusMsgType got_type{};
    std::thread consumer([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (reader.TryRecv(got_type, got) == transport::BusRecvResult::Message) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    ASSERT_TRUE(sender.TrySend(BusMsgType::Tick, big, deadline));
    consumer.join();
    EXPECT_EQ(got_type, BusMsgType::Tick);
    EXPECT_EQ(got, big);
}

TEST(SpscBus, NonBlockingSendDoesNotWaitWhenRingIsFull) {
    QueueRegistry reg;
    auto created = reg.Create("pop-nowait");
    SpscBus out(reg.FindOut(created.out_queue));
    const std::size_t nbytes =
        transport::kMaxBusChunkBytes * (transport::kQueueCapacity - 1) + 1;
    std::vector<std::uint8_t> big(nbytes, 0xab);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(out.TrySend(BusMsgType::Tick, big));
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(200));
}

TEST(SpscBus, WaitingSendFailsWhenDeadlinePasses) {
    QueueRegistry reg;
    auto created = reg.Create("pop-deadline");
    SpscBus out(reg.FindOut(created.out_queue));
    const std::size_t nbytes =
        transport::kMaxBusChunkBytes * (transport::kQueueCapacity - 1) + 1;
    std::vector<std::uint8_t> big(nbytes, 0x11);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(out.TrySend(BusMsgType::Tick, big, deadline));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, std::chrono::milliseconds(50));
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));
}
