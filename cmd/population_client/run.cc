#include "population_venue/log/log.hpp"
#include "run.hpp"

#include "session.hpp"

#include "transport/control.hpp"
#include "transport/shm_queue.hpp"
#include "transport/uds_control.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using transport::AckStatus;
using transport::AttachShmQueue;
using transport::CreateQueues;
using transport::RequestCreateQueues;
using transport::UdsConn;
using transport::UdsConnect;
using transport::WaitSessionEnded;

namespace client {

int Run(const Config& cfg) {
    const int raw = UdsConnect(cfg.uds_path);
    if (raw < 0) {
        LOG_ERROR("connect failed {}", cfg.uds_path);
        return EXIT_FAILURE;
    }
    UdsConn conn(raw);

    std::vector<PopHandle> pops;
    pops.reserve(static_cast<std::size_t>(cfg.n_pops));
    for (int i = 0; i < cfg.n_pops; ++i) {
        const std::string id = "take" + std::to_string(i);
        const auto ack = RequestCreateQueues(conn, CreateQueues{.population_id = id});
        if (ack.status != AckStatus::Ok) {
            LOG_ERROR("Create {} failed: {}", id, ack.message);
            return EXIT_FAILURE;
        }
        auto out_map = AttachShmQueue(ack.out_queue);
        auto in_map = AttachShmQueue(ack.in_queue);
        if (!out_map || !in_map) {
            LOG_ERROR("shm attach failed for {}", id);
            return EXIT_FAILURE;
        }
        PopHandle pop;
        pop.population_id = id;
        pop.out_name = ack.out_queue;
        pop.in_name = ack.in_queue;
        pop.out_map = std::move(*out_map);
        pop.in_map = std::move(*in_map);
        pops.push_back(std::move(pop));
        LOG_INFO("created {} out={} in={}", id, pops.back().out_name, pops.back().in_name);
    }

    EndedHub hub;
    std::atomic<int> remaining{cfg.n_pops};
    std::thread control([&] {
        while (remaining.load(std::memory_order_relaxed) > 0) {
            transport::SessionEnded ended;
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
        workers.emplace_back([&pop, &cfg, &hub] { (void)RunPopulationLoop(pop, cfg, hub); });
    }
    for (auto& t : workers) {
        t.join();
    }
    control.join();

    const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();

    int ok_n = 0;
    int ticks = 0;
    int fills = 0;
    for (const auto& pop : pops) {
        if (!pop.ok) {
            LOG_ERROR("FAIL {}: {}", pop.population_id, pop.error);
            continue;
        }
        ++ok_n;
        ticks += pop.stats.ticks;
        fills += pop.stats.fills;
        const double avg = pop.stats.fills > 0
                               ? static_cast<double>(pop.stats.sum_fill_delay_ns) / pop.stats.fills
                               : 0.0;
        const std::uint64_t min_d = pop.stats.fills > 0 ? pop.stats.min_fill_delay_ns : 0;
        LOG_INFO("OK {} ticks={} orders={} fills={} cancels={} delay_ns(min/avg/max)={}/{:.0f}/{}",
                 pop.population_id, pop.stats.ticks, pop.stats.orders_sent, pop.stats.fills,
                 pop.stats.cancels, min_d, avg, pop.stats.max_fill_delay_ns);
    }

    const double ticks_per_s = wall_us > 0 ? (static_cast<double>(ticks) * 1'000'000.0) / wall_us : 0.0;
    LOG_INFO("summary pops_ok={}/{} ticks={} fills={} wall_ms={:.1f} ticks_per_s={:.0f}", ok_n,
             cfg.n_pops, ticks, fills, wall_us / 1000.0, ticks_per_s);
    return ok_n == cfg.n_pops ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace client
