#include "transport/bus_framing.hpp"
#include "transport/queue_registry.hpp"
#include "transport/spsc_bus.hpp"

#include <gtest/gtest.h>

#include <numeric>

using transport::AckStatus;
using transport::BusError;
using transport::BusMsgType;
using transport::BusOpen;
using transport::BusOpenAck;
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

    ASSERT_TRUE(in.TrySend(BusMsgType::Open, EncodeBusOpen(BusOpen{.population_id = "pop-1"})));
    {
        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        ASSERT_EQ(in.TryRecv(type, payload), BusRecvResult::Message);
        EXPECT_EQ(type, BusMsgType::Open);
        BusOpen open;
        ASSERT_TRUE(DecodeBusOpen(payload.data(), payload.size(), open));
        EXPECT_EQ(open.population_id, "pop-1");
    }

    ASSERT_TRUE(out.TrySend(BusMsgType::OpenAck, EncodeBusOpenAck(MakeBusOpenOk("pop-1"))));
    ASSERT_TRUE(out.TrySend(BusMsgType::Tick, EncodeBusTick(BusTick{.pop_ts_ns = 100})));

    {
        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        ASSERT_EQ(out.TryRecv(type, payload), BusRecvResult::Message);
        EXPECT_EQ(type, BusMsgType::OpenAck);
        BusOpenAck open_ack;
        ASSERT_TRUE(DecodeBusOpenAck(payload.data(), payload.size(), open_ack));
        EXPECT_EQ(open_ack.status, AckStatus::Ok);

        ASSERT_EQ(out.TryRecv(type, payload), BusRecvResult::Message);
        EXPECT_EQ(type, BusMsgType::Tick);
        BusTick tick;
        ASSERT_TRUE(DecodeBusTick(payload.data(), payload.size(), tick));
        EXPECT_EQ(tick.pop_ts_ns, 100u);
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
