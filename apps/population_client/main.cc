// External Engine client: UDS CreateQueues + POSIX shm SPSC.
// Strategy: each Tick take best ask (Buy IOC) and best bid (Sell IOC).
// Usage: population_client [uds_path] [dataset.jsonl]
// dataset_path is opened by the venue process (must be readable there).

#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/control.hpp"
#include "transport/shm_queue.hpp"
#include "transport/spsc_bus.hpp"
#include "transport/uds_control.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using transport::AckStatus;
using transport::AttachShmQueue;
using transport::BusAssetRole;
using transport::BusBookUpdate;
using transport::BusComponent;
using transport::BusMsgType;
using transport::BusOpen;
using transport::BusOpenAck;
using transport::BusOrder;
using transport::BusOrderBatch;
using transport::BusOrdersAck;
using transport::BusRecvResult;
using transport::BusSide;
using transport::BusSlotConfig;
using transport::BusTick;
using transport::BusTimeInForce;
using transport::CreateQueues;
using transport::DecodeBusEnd;
using transport::DecodeBusOpenAck;
using transport::DecodeBusOrdersAck;
using transport::DecodeBusTick;
using transport::EncodeBusAdvance;
using transport::EncodeBusClose;
using transport::EncodeBusOpen;
using transport::EncodeBusOrderBatch;
using transport::RequestCreateQueues;
using transport::SessionEnded;
using transport::SessionEndStatus;
using transport::SpscBus;
using transport::UdsConn;
using transport::UdsConnect;
using transport::WaitSessionEnded;

bool RecvMessage(SpscBus& bus, BusMsgType& type, std::vector<std::uint8_t>& payload,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(10'000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto r = bus.TryRecv(type, payload);
        if (r == BusRecvResult::Message) {
            return true;
        }
        if (r == BusRecvResult::ProtocolError) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

BusOpen MakeOpen(const std::string& population_id, const std::string& dataset) {
    BusOpen open;
    open.population_id = population_id;
    open.components.push_back(BusComponent{
        .component_id = "SPOT",
        .role = BusAssetRole::Spot,
        .dataset_path = dataset,
        .class_code = "TQBR",
        .sec_code = "SBER",
        .tick_size = "0.01",
    });
    open.slots.push_back(BusSlotConfig{
        .slot_id = 1,
        .md_lag_ns = 0,
        .delta_ns = 1,
        .impact_lambda = "0",
    });
    return open;
}

const BusBookUpdate* FirstBook(const BusTick& tick) {
    if (!tick.slots.empty() && !tick.slots[0].books.empty()) {
        return &tick.slots[0].books[0];
    }
    if (!tick.books.empty()) {
        return &tick.books[0];
    }
    return nullptr;
}

BusOrderBatch MakeTakeBatch(const BusTick& tick, std::uint64_t& next_oid) {
    BusOrderBatch batch;
    const BusBookUpdate* book = FirstBook(tick);
    if (book == nullptr) {
        return batch;
    }
    const std::uint32_t slot_id = tick.slots.empty() ? 1u : tick.slots[0].slot_id;
    if (!book->asks.empty() && book->asks[0].qty > 0) {
        batch.orders.push_back(BusOrder{
            .slot_id = slot_id,
            .client_order_id = next_oid++,
            .component_id = book->component_id.empty() ? "SPOT" : book->component_id,
            .side = BusSide::Buy,
            .price = book->asks[0].price,
            .qty = 1,
            .tif = BusTimeInForce::Ioc,
        });
    }
    if (!book->bids.empty() && book->bids[0].qty > 0) {
        batch.orders.push_back(BusOrder{
            .slot_id = slot_id,
            .client_order_id = next_oid++,
            .component_id = book->component_id.empty() ? "SPOT" : book->component_id,
            .side = BusSide::Sell,
            .price = book->bids[0].price,
            .qty = 1,
            .tif = BusTimeInForce::Ioc,
        });
    }
    return batch;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string uds_path =
        (argc >= 2 && argv[1][0] != '\0')
            ? argv[1]
            : (std::getenv("POPULATION_UDS") != nullptr ? std::getenv("POPULATION_UDS")
                                                        : "/tmp/cmf-population.sock");
    const std::string dataset =
        (argc >= 3 && argv[2][0] != '\0') ? argv[2] : "testdata/population/smoke.jsonl";

    std::fprintf(stderr, "client: uds=%s dataset=%s\n", uds_path.c_str(), dataset.c_str());

    const int raw = UdsConnect(uds_path);
    if (raw < 0) {
        std::fprintf(stderr, "client: connect failed %s\n", uds_path.c_str());
        return EXIT_FAILURE;
    }
    UdsConn conn(raw);

    const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = "take"});
    if (ack.status != AckStatus::Ok) {
        std::fprintf(stderr, "client: Create failed: %s\n", ack.message.c_str());
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "client: queues out=%s in=%s\n", ack.out_queue.c_str(),
                 ack.in_queue.c_str());

    auto out_map = AttachShmQueue(ack.out_queue);
    auto in_map = AttachShmQueue(ack.in_queue);
    if (!out_map || !in_map) {
        std::fprintf(stderr, "client: shm attach failed\n");
        return EXIT_FAILURE;
    }
    SpscBus out(out_map->get());
    SpscBus in(in_map->get());

    if (!in.TrySend(transport::BusMsgType::Open, EncodeBusOpen(MakeOpen("take", dataset)))) {
        std::fprintf(stderr, "client: Open send failed\n");
        return EXIT_FAILURE;
    }

    BusMsgType type{};
    std::vector<std::uint8_t> payload;
    if (!RecvMessage(out, type, payload) || type != BusMsgType::OpenAck) {
        std::fprintf(stderr, "client: expected OpenAck\n");
        return EXIT_FAILURE;
    }
    BusOpenAck open_ack;
    if (!DecodeBusOpenAck(payload.data(), payload.size(), open_ack) ||
        open_ack.status != AckStatus::Ok) {
        std::fprintf(stderr, "client: OpenAck error: %s\n", open_ack.message.c_str());
        return EXIT_FAILURE;
    }

    int ticks = 0;
    int orders_sent = 0;
    int trades_seen = 0;
    std::uint64_t next_oid = 1;

    for (;;) {
        if (!RecvMessage(out, type, payload)) {
            std::fprintf(stderr, "client: recv timeout/error after %d ticks\n", ticks);
            return EXIT_FAILURE;
        }
        if (type == BusMsgType::End) {
            if (!DecodeBusEnd(payload.data(), payload.size())) {
                std::fprintf(stderr, "client: bad End\n");
                return EXIT_FAILURE;
            }
            break;
        }
        if (type == BusMsgType::Error) {
            std::fprintf(stderr, "client: venue Error\n");
            return EXIT_FAILURE;
        }
        if (type != BusMsgType::Tick) {
            std::fprintf(stderr, "client: unexpected type %u\n", static_cast<unsigned>(type));
            return EXIT_FAILURE;
        }

        BusTick tick;
        if (!DecodeBusTick(payload.data(), payload.size(), tick)) {
            std::fprintf(stderr, "client: bad Tick\n");
            return EXIT_FAILURE;
        }
        ++ticks;
        for (const auto& slot : tick.slots) {
            trades_seen += static_cast<int>(slot.trades.size());
        }

        BusOrderBatch batch = MakeTakeBatch(tick, next_oid);
        if (!batch.orders.empty()) {
            orders_sent += static_cast<int>(batch.orders.size());
            if (!in.TrySend(BusMsgType::OrderBatch, EncodeBusOrderBatch(batch))) {
                std::fprintf(stderr, "client: OrderBatch send failed\n");
                return EXIT_FAILURE;
            }
            if (!RecvMessage(out, type, payload) || type != BusMsgType::OrdersAck) {
                std::fprintf(stderr, "client: expected OrdersAck\n");
                return EXIT_FAILURE;
            }
            BusOrdersAck orders_ack;
            if (!DecodeBusOrdersAck(payload.data(), payload.size(), orders_ack) ||
                orders_ack.status != AckStatus::Ok) {
                std::fprintf(stderr, "client: OrdersAck reject: %s\n", orders_ack.message.c_str());
                return EXIT_FAILURE;
            }
        }

        if (!in.TrySend(BusMsgType::Advance, EncodeBusAdvance())) {
            std::fprintf(stderr, "client: Advance send failed\n");
            return EXIT_FAILURE;
        }
    }

    SessionEnded ended;
    if (!WaitSessionEnded(conn, ended)) {
        std::fprintf(stderr, "client: SessionEnded missing\n");
        return EXIT_FAILURE;
    }
    (void)in.TrySend(BusMsgType::Close, EncodeBusClose());

    if (ended.status != SessionEndStatus::Ok) {
        std::fprintf(stderr, "client: SessionEnded fatal: %s\n", ended.message.c_str());
        return EXIT_FAILURE;
    }
    if (ticks < 1) {
        std::fprintf(stderr, "client: no ticks\n");
        return EXIT_FAILURE;
    }

    std::fprintf(stderr,
                 "client: OK ticks=%d orders=%d trades_seen=%d session=%s\n", ticks, orders_sent,
                 trades_seen, ended.population_id.c_str());
    return EXIT_SUCCESS;
}
