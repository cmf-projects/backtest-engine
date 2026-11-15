#include "transport/control.hpp"

#include <gtest/gtest.h>

using transport::AckStatus;
using transport::CreateQueues;
using transport::ErrorCode;
using transport::MakeCreateOk;
using transport::MakeCreateReject;
using transport::MakeSessionEndedFatal;
using transport::MakeSessionEndedOk;
using transport::SessionEndStatus;

TEST(Control, CreateQueuesAndAckCarryPopulationId) {
    CreateQueues req{.population_id = "pop-1"};
    auto ack = MakeCreateOk(req.population_id, "out-a", "in-a");

    EXPECT_EQ(ack.population_id, "pop-1");
    EXPECT_EQ(ack.status, AckStatus::Ok);
    EXPECT_EQ(ack.out_queue, "out-a");
    EXPECT_EQ(ack.in_queue, "in-a");
    EXPECT_EQ(ack.reason_code, ErrorCode::Unspecified);
}

TEST(Control, CreateReject) {
    auto ack = MakeCreateReject("pop-2", ErrorCode::NoQuota, "no free slot");

    EXPECT_EQ(ack.population_id, "pop-2");
    EXPECT_EQ(ack.status, AckStatus::Error);
    EXPECT_EQ(ack.reason_code, ErrorCode::NoQuota);
    EXPECT_EQ(ack.message, "no free slot");
    EXPECT_TRUE(ack.out_queue.empty());
}

TEST(Control, SessionEndedFatal) {
    auto ended =
        MakeSessionEndedFatal("pop-3", ErrorCode::DrainTimeout, "engine did not Close");

    EXPECT_EQ(ended.population_id, "pop-3");
    EXPECT_EQ(ended.status, SessionEndStatus::Fatal);
    EXPECT_EQ(ended.reason_code, ErrorCode::DrainTimeout);
    EXPECT_FALSE(ended.message.empty());
}

TEST(Control, SessionEndedOkClientClose) {
    auto ended = MakeSessionEndedOk("pop-4", ErrorCode::ClientClose, "client Close");

    EXPECT_EQ(ended.population_id, "pop-4");
    EXPECT_EQ(ended.status, SessionEndStatus::Ok);
    EXPECT_EQ(ended.reason_code, ErrorCode::ClientClose);
}
