#include "population_venue/log/log.hpp"
#include "session.hpp"

#include "transport/bus_framing.hpp"
#include "transport/spsc_bus.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using transport::AckStatus;
using transport::BusMsgType;
using transport::BusOpenAck;
using transport::BusOrderBatch;
using transport::BusOrdersAck;
using transport::BusTick;
using transport::DecodeBusEnd;
using transport::DecodeBusOpenAck;
using transport::DecodeBusOrdersAck;
using transport::DecodeBusTick;
using transport::EncodeBusAdvanceInto;
using transport::EncodeBusCloseInto;
using transport::EncodeBusOpenInto;
using transport::EncodeBusOrderBatchInto;
using transport::SessionEndStatus;
using transport::SpscBus;

namespace client {
namespace {

[[nodiscard]] bool RecvMessage(transport::SpscBus& bus, transport::BusMsgType& type,
                               std::vector<std::uint8_t>& payload,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(8'000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto r = bus.TryRecv(type, payload);
        if (r == transport::BusRecvResult::Message) {
            return true;
        }
        if (r == transport::BusRecvResult::ProtocolError) {
            return false;
        }
        std::this_thread::yield();
    }
    return false;
}

void NoteBookComponents(const BusTick& tick, std::unordered_set<std::string>& seen) {
    auto note = [&](const std::string& id) {
        if (id.empty() || !seen.insert(id).second) {
            return;
        }
        LOG_INFO("book component={}", id);
    };
    for (std::uint32_t i = 0; i < tick.n_books; ++i) {
        note(tick.books[i].component_id);
    }
    for (const auto& slot : tick.slots) {
        for (std::uint32_t i = 0; i < slot.n_books; ++i) {
            note(slot.books[i].component_id);
        }
    }
}

}  // namespace

void EndedHub::Publish(transport::SessionEnded ended) {
    std::lock_guard lock(mu);
    by_id[ended.population_id] = std::move(ended);
    cv.notify_all();
}

bool EndedHub::Wait(const std::string& population_id, transport::SessionEnded& out,
                    std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu);
    if (!cv.wait_for(lock, timeout, [&] {
            return failed.load(std::memory_order_relaxed) || by_id.count(population_id) > 0;
        })) {
        return false;
    }
    if (failed.load(std::memory_order_relaxed)) {
        return false;
    }
    out = by_id[population_id];
    return true;
}

bool RunPopulationLoop(PopHandle& pop, const Config& cfg, EndedHub& hub) {
    SpscBus out(pop.out_map.get());
    SpscBus in(pop.in_map.get());
    std::vector<std::uint8_t> encode_buf;
    std::vector<std::uint8_t> payload;
    encode_buf.reserve(512);
    payload.reserve(512);

    EncodeBusOpenInto(MakeOpen(pop.population_id, cfg), encode_buf);
    if (!in.TrySend(BusMsgType::Open, encode_buf)) {
        pop.error = "Open send failed";
        return false;
    }

    BusMsgType type{};
    if (!RecvMessage(out, type, payload) || type != BusMsgType::OpenAck) {
        pop.error = "expected OpenAck";
        return false;
    }
    BusOpenAck open_ack;
    if (!DecodeBusOpenAck(payload.data(), payload.size(), open_ack) ||
        open_ack.status != AckStatus::Ok) {
        pop.error = "OpenAck error: " + open_ack.message;
        return false;
    }

    std::uint64_t next_oid = 1;
    std::unordered_set<std::string> seen_books;
    DensePending pending;
    pending.Reserve(32'768);
    BusTick tick;
    BusOrderBatch batch;
    batch.orders.reserve(8);
    std::vector<RememberedBook> books;
    books.reserve(4);
    BusOrdersAck orders_ack;

    for (;;) {
        if (!RecvMessage(out, type, payload)) {
            pop.error = "recv timeout/error";
            return false;
        }
        if (type == BusMsgType::End) {
            if (!DecodeBusEnd(payload.data(), payload.size())) {
                pop.error = "bad End";
                return false;
            }
            break;
        }
        if (type == BusMsgType::Error) {
            pop.error = "venue Error";
            return false;
        }
        if (type != BusMsgType::Tick) {
            pop.error = "unexpected type";
            return false;
        }

        if (!DecodeBusTick(payload.data(), payload.size(), tick)) {
            pop.error = "bad Tick";
            return false;
        }
        ++pop.stats.ticks;
        NoteBookComponents(tick, seen_books);
        NoteTrades(tick, pending, pop.stats);

        MakeTakeBatch(tick, pop.stats.ticks, cfg.order_every_ticks, next_oid, pending, batch,
                      books);
        if (!batch.orders.empty()) {
            pop.stats.orders_sent += static_cast<int>(batch.orders.size());
            EncodeBusOrderBatchInto(batch, encode_buf);
            if (!in.TrySend(BusMsgType::OrderBatch, encode_buf)) {
                pop.error = "OrderBatch send failed";
                return false;
            }
            if (!RecvMessage(out, type, payload) || type != BusMsgType::OrdersAck) {
                pop.error = "expected OrdersAck";
                return false;
            }
            if (!DecodeBusOrdersAck(payload.data(), payload.size(), orders_ack) ||
                orders_ack.status != AckStatus::Ok) {
                pop.error = "OrdersAck reject: " + orders_ack.message;
                return false;
            }
        }

        EncodeBusAdvanceInto(encode_buf);
        if (!in.TrySend(BusMsgType::Advance, encode_buf)) {
            pop.error = "Advance send failed";
            return false;
        }
    }

    transport::SessionEnded ended;
    if (!hub.Wait(pop.population_id, ended, std::chrono::milliseconds(8'000))) {
        pop.error = "SessionEnded timeout";
        return false;
    }
    EncodeBusCloseInto(encode_buf);
    (void)in.TrySend(BusMsgType::Close, encode_buf);

    if (ended.status != SessionEndStatus::Ok) {
        pop.error = "SessionEnded fatal: " + ended.message;
        return false;
    }
    if (pop.stats.ticks < 1) {
        pop.error = "no ticks";
        return false;
    }
    for (const char* id : {"SPOT", "FUT", "OPT"}) {
        if (!seen_books.count(id)) {
            pop.error = std::string("no book events for ") + id;
            return false;
        }
    }
    pop.ok = true;
    return true;
}

}  // namespace client
