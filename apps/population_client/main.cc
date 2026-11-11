// External Engine client: UDS CreateQueues + POSIX shm SPSC.
// Strategy: each Tick take best ask (Buy IOC) and best bid (Sell IOC) on every book.
// Usage:
//   population_client [uds_path] [dataset_or_dir] [n_pops]
// dataset_or_dir:
//   - directory with spot.jsonl + fut.jsonl + option.jsonl (bundle), or
//   - single JSONL (1-component SPOT).
// n_pops: concurrent populations on one UDS (default 1). Env CLIENT_POPS overrides.

#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/control.hpp"
#include "transport/shm_queue.hpp"
#include "transport/spsc_bus.hpp"
#include "transport/uds_control.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>

namespace {

using transport::AckStatus;
using transport::AttachShmQueue;
using transport::BusAssetRole;
using transport::BusBookUpdate;
using transport::BusComponent;
using transport::BusExecType;
using transport::BusMsgType;
using transport::BusOpen;
using transport::BusOpenAck;
using transport::BusOrder;
using transport::BusOrderBatch;
using transport::BusOrdersAck;
using transport::BusPrice;
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
using transport::EncodeBusAdvanceInto;
using transport::EncodeBusCloseInto;
using transport::EncodeBusOpenInto;
using transport::EncodeBusOrderBatchInto;
using transport::RequestCreateQueues;
using transport::SessionEnded;
using transport::SessionEndStatus;
using transport::ShmQueueMapping;
using transport::SpscBus;
using transport::UdsConn;
using transport::UdsConnect;
using transport::WaitSessionEnded;

struct PendingOrder {
    std::uint64_t submit_pop_ts_ns = 0;
    std::string component_id;
    BusSide side = BusSide::Unspecified;
    BusPrice price;
};

// Monotonic client_order_id → slot; no per-insert heap nodes.
struct DensePending {
    struct Slot {
        bool live = false;
        PendingOrder order;
    };

    std::vector<Slot> slots;  // index == oid (0 unused)

    void Reserve(std::size_t n) { slots.reserve(n); }

    void Put(std::uint64_t oid, PendingOrder order) {
        if (oid >= slots.size()) {
            slots.resize(oid + 1);
        }
        slots[oid].order = std::move(order);
        slots[oid].live = true;
    }

    [[nodiscard]] PendingOrder* Find(std::uint64_t oid) {
        if (oid >= slots.size() || !slots[oid].live) {
            return nullptr;
        }
        return &slots[oid].order;
    }

    void Erase(std::uint64_t oid) {
        if (oid < slots.size()) {
            slots[oid].live = false;
        }
    }
};

struct RunStats {
    int ticks = 0;
    int orders_sent = 0;
    int fills = 0;
    int cancels = 0;
    std::uint64_t sum_fill_delay_ns = 0;
    std::uint64_t max_fill_delay_ns = 0;
    std::uint64_t min_fill_delay_ns = UINT64_MAX;
};

struct EndedHub {
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::string, SessionEnded> by_id;
    std::atomic<bool> failed{false};

    void Publish(SessionEnded ended) {
        std::lock_guard lock(mu);
        by_id[ended.population_id] = std::move(ended);
        cv.notify_all();
    }

    [[nodiscard]] bool Wait(const std::string& population_id, SessionEnded& out,
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
};

[[nodiscard]] bool IsDir(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool RecvMessage(SpscBus& bus, BusMsgType& type, std::vector<std::uint8_t>& payload,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(60'000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto r = bus.TryRecv(type, payload);
        if (r == BusRecvResult::Message) {
            return true;
        }
        if (r == BusRecvResult::ProtocolError) {
            return false;
        }
        std::this_thread::yield();
    }
    return false;
}

BusOpen MakeOpenSingle(const std::string& population_id, const std::string& dataset) {
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

BusOpen MakeOpenBundle(const std::string& population_id, const std::string& dir) {
    const std::string base = dir.back() == '/' ? dir : dir + "/";
    BusOpen open;
    open.population_id = population_id;
    open.components.push_back(BusComponent{
        .component_id = "SPOT",
        .role = BusAssetRole::Spot,
        .dataset_path = base + "spot.jsonl",
        .class_code = "TQBR",
        .sec_code = "SBER",
        .tick_size = "0.01",
    });
    open.components.push_back(BusComponent{
        .component_id = "FUT",
        .role = BusAssetRole::Future,
        .dataset_path = base + "fut.jsonl",
        .class_code = "SPBFUT",
        .sec_code = "SBER_FUT",
        .tick_size = "0.01",
    });
    open.components.push_back(BusComponent{
        .component_id = "OPT",
        .role = BusAssetRole::Option,
        .dataset_path = base + "option.jsonl",
        .class_code = "SPBOPT",
        .sec_code = "SBER_OPT",
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

void CollectBooks(const BusTick& tick, std::vector<const BusBookUpdate*>& out) {
    out.clear();
    if (!tick.slots.empty()) {
        for (const auto& book : tick.slots[0].books) {
            out.push_back(&book);
        }
    }
    if (out.empty()) {
        for (const auto& book : tick.books) {
            out.push_back(&book);
        }
    }
}

void MakeTakeBatch(const BusTick& tick, std::uint64_t& next_oid, DensePending& pending,
                   BusOrderBatch& batch, std::vector<const BusBookUpdate*>& books) {
    batch.orders.clear();
    batch.cancels.clear();
    CollectBooks(tick, books);
    if (books.empty()) {
        return;
    }
    const std::uint32_t slot_id = tick.slots.empty() ? 1u : tick.slots[0].slot_id;

    auto push = [&](BusSide side, const BusBookUpdate& book, const BusPrice& px) {
        const std::uint64_t oid = next_oid++;
        batch.orders.push_back(BusOrder{
            .slot_id = slot_id,
            .client_order_id = oid,
            .component_id = book.component_id,
            .side = side,
            .price = px,
            .qty = 1,
            .tif = BusTimeInForce::Ioc,
        });
        pending.Put(oid, PendingOrder{
                             .submit_pop_ts_ns = tick.pop_ts_ns,
                             .component_id = book.component_id,
                             .side = side,
                             .price = px,
                         });
    };

    auto price_ok = [](const BusPrice& px) {
        return px.whole > 0 || px.frac > 0;
    };

    for (const BusBookUpdate* book : books) {
        if (book == nullptr) {
            continue;
        }
        if (!book->asks.empty() && book->asks[0].qty > 0 && price_ok(book->asks[0].price)) {
            push(BusSide::Buy, *book, book->asks[0].price);
        }
        if (!book->bids.empty() && book->bids[0].qty > 0 && price_ok(book->bids[0].price)) {
            push(BusSide::Sell, *book, book->bids[0].price);
        }
    }
}

void NoteTrades(const BusTick& tick, DensePending& pending, RunStats& stats) {
    for (const auto& slot : tick.slots) {
        for (const auto& tr : slot.trades) {
            if (tr.exec_type == BusExecType::Cancel) {
                ++stats.cancels;
                pending.Erase(tr.client_order_id);
                continue;
            }
            if (tr.exec_type != BusExecType::Fill && tr.exec_type != BusExecType::Unspecified) {
                continue;
            }
            ++stats.fills;
            PendingOrder* po = pending.Find(tr.client_order_id);
            if (po == nullptr) {
                continue;
            }
            const std::uint64_t delay = tick.pop_ts_ns >= po->submit_pop_ts_ns
                                            ? tick.pop_ts_ns - po->submit_pop_ts_ns
                                            : 0;
            stats.sum_fill_delay_ns += delay;
            if (delay > stats.max_fill_delay_ns) {
                stats.max_fill_delay_ns = delay;
            }
            if (delay < stats.min_fill_delay_ns) {
                stats.min_fill_delay_ns = delay;
            }
            if (tr.leaves == 0) {
                pending.Erase(tr.client_order_id);
            }
        }
    }
}

struct PopHandle {
    std::string population_id;
    std::string out_name;
    std::string in_name;
    ShmQueueMapping out_map;
    ShmQueueMapping in_map;
    RunStats stats;
    bool ok = false;
    std::string error;
};

[[nodiscard]] bool RunPopulationLoop(PopHandle& pop, const std::string& dataset, bool bundle,
                                     EndedHub& hub) {
    SpscBus out(pop.out_map.get());
    SpscBus in(pop.in_map.get());
    std::vector<std::uint8_t> encode_buf;
    std::vector<std::uint8_t> payload;
    encode_buf.reserve(512);
    payload.reserve(512);

    const BusOpen open =
        bundle ? MakeOpenBundle(pop.population_id, dataset) : MakeOpenSingle(pop.population_id, dataset);
    EncodeBusOpenInto(open, encode_buf);
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
    DensePending pending;
    pending.Reserve(32'768);
    BusTick tick;
    BusOrderBatch batch;
    batch.orders.reserve(8);
    std::vector<const BusBookUpdate*> books;
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
        NoteTrades(tick, pending, pop.stats);

        MakeTakeBatch(tick, next_oid, pending, batch, books);
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

    SessionEnded ended;
    if (!hub.Wait(pop.population_id, ended, std::chrono::milliseconds(120'000))) {
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
    pop.ok = true;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string uds_path =
        (argc >= 2 && argv[1][0] != '\0')
            ? argv[1]
            : (std::getenv("POPULATION_UDS") != nullptr ? std::getenv("POPULATION_UDS")
                                                        : "/tmp/cmf-population.sock");
    const std::string dataset =
        (argc >= 3 && argv[2][0] != '\0') ? argv[2] : "testdata/population/noise";

    int n_pops = 1;
    if (const char* env = std::getenv("CLIENT_POPS"); env != nullptr && env[0] != '\0') {
        n_pops = std::atoi(env);
    } else if (argc >= 4 && argv[3][0] != '\0') {
        n_pops = std::atoi(argv[3]);
    }
    if (n_pops < 1) {
        std::fprintf(stderr, "client: n_pops must be >= 1\n");
        return EXIT_FAILURE;
    }

    const bool bundle = IsDir(dataset);
    std::fprintf(stderr, "client: uds=%s dataset=%s mode=%s pops=%d\n", uds_path.c_str(),
                 dataset.c_str(), bundle ? "bundle(spot/fut/option)" : "single", n_pops);

    const int raw = UdsConnect(uds_path);
    if (raw < 0) {
        std::fprintf(stderr, "client: connect failed %s\n", uds_path.c_str());
        return EXIT_FAILURE;
    }
    UdsConn conn(raw);

    std::vector<PopHandle> pops;
    pops.reserve(static_cast<std::size_t>(n_pops));
    for (int i = 0; i < n_pops; ++i) {
        const std::string id = "take" + std::to_string(i);
        const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = id});
        if (ack.status != AckStatus::Ok) {
            std::fprintf(stderr, "client: Create %s failed: %s\n", id.c_str(), ack.message.c_str());
            return EXIT_FAILURE;
        }
        auto out_map = AttachShmQueue(ack.out_queue);
        auto in_map = AttachShmQueue(ack.in_queue);
        if (!out_map || !in_map) {
            std::fprintf(stderr, "client: shm attach failed for %s\n", id.c_str());
            return EXIT_FAILURE;
        }
        PopHandle pop;
        pop.population_id = id;
        pop.out_name = ack.out_queue;
        pop.in_name = ack.in_queue;
        pop.out_map = std::move(*out_map);
        pop.in_map = std::move(*in_map);
        pops.push_back(std::move(pop));
        std::fprintf(stderr, "client: created %s out=%s in=%s\n", id.c_str(),
                     pops.back().out_name.c_str(), pops.back().in_name.c_str());
    }

    EndedHub hub;
    std::atomic<int> remaining{n_pops};
    std::thread control([&] {
        while (remaining.load(std::memory_order_relaxed) > 0) {
            SessionEnded ended;
            if (!WaitSessionEnded(conn, ended)) {
                hub.failed.store(true, std::memory_order_relaxed);
                hub.cv.notify_all();
                return;
            }
            hub.Publish(std::move(ended));
            remaining.fetch_sub(1, std::memory_order_relaxed);
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(pops.size());
    for (auto& pop : pops) {
        workers.emplace_back([&pop, &dataset, bundle, &hub] {
            (void)RunPopulationLoop(pop, dataset, bundle, hub);
        });
    }
    for (auto& t : workers) {
        t.join();
    }
    control.join();

    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    int ok_n = 0;
    int ticks = 0;
    int fills = 0;
    for (const auto& pop : pops) {
        if (!pop.ok) {
            std::fprintf(stderr, "client: FAIL %s: %s\n", pop.population_id.c_str(),
                         pop.error.c_str());
            continue;
        }
        ++ok_n;
        ticks += pop.stats.ticks;
        fills += pop.stats.fills;
        const double avg =
            pop.stats.fills > 0
                ? static_cast<double>(pop.stats.sum_fill_delay_ns) / pop.stats.fills
                : 0.0;
        const std::uint64_t min_d = pop.stats.fills > 0 ? pop.stats.min_fill_delay_ns : 0;
        std::fprintf(stderr,
                     "client: OK %s ticks=%d orders=%d fills=%d cancels=%d "
                     "delay_ns(min/avg/max)=%llu/%.0f/%llu\n",
                     pop.population_id.c_str(), pop.stats.ticks, pop.stats.orders_sent,
                     pop.stats.fills, pop.stats.cancels, static_cast<unsigned long long>(min_d),
                     avg, static_cast<unsigned long long>(pop.stats.max_fill_delay_ns));
    }

    std::fprintf(stderr, "client: summary pops_ok=%d/%d ticks=%d fills=%d wall_ms=%lld\n", ok_n,
                 n_pops, ticks, fills, static_cast<long long>(wall_ms));
    return ok_n == n_pops ? EXIT_SUCCESS : EXIT_FAILURE;
}
