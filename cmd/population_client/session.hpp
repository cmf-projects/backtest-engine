#pragma once

#include "config.hpp"
#include "orders.hpp"

#include "transport/control.hpp"
#include "transport/shm_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>

namespace client {

struct EndedHub {
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::string, transport::SessionEnded> by_id;
    std::atomic<bool> failed{false};

    void Publish(transport::SessionEnded ended);

    [[nodiscard]] bool Wait(const std::string& population_id, transport::SessionEnded& out,
                            std::chrono::milliseconds timeout);
};

struct PopHandle {
    std::string population_id;
    std::string out_name;
    std::string in_name;
    transport::ShmQueueMapping out_map;
    transport::ShmQueueMapping in_map;
    RunStats stats;
    bool ok = false;
    std::string error;
};

[[nodiscard]] bool RunPopulationLoop(PopHandle& pop, const Config& cfg, EndedHub& hub);

}  // namespace client
