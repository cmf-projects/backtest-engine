#pragma once

// In-process registry of per-population SPSC pairs (venue process).
// CreateAck.out_queue / in_queue are keys into this registry.
// Cross-process shm comes later; API stays name-based.

#include "transport/control.hpp"
#include "transport/spsc_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace transport {

inline constexpr std::size_t kQueueCapacity = 1024;
// Bytes per ring slot body; logical bus messages may span many slots.
inline constexpr std::size_t kQueuePayloadBytes = 2040;

struct QueueMessage {
    std::uint32_t size = 0;
    std::uint8_t bytes[kQueuePayloadBytes]{};
};

static_assert(std::is_trivially_copyable_v<QueueMessage>);

using PopulationQueue = SpscRing<QueueMessage, kQueueCapacity>;

struct QueuePair {
    std::string population_id;
    std::string out_name;  // venue → engine
    std::string in_name;   // engine → venue
    PopulationQueue out;
    PopulationQueue in;
};

class QueueRegistry {
public:
    [[nodiscard]] CreateAck Create(const std::string& population_id) {
        if (population_id.empty()) {
            return MakeCreateReject("", ErrorCode::CreateFailed, "empty population_id");
        }

        std::lock_guard lock(mu_);
        if (by_pop_.find(population_id) != by_pop_.end()) {
            return MakeCreateReject(population_id, ErrorCode::CreateFailed,
                                    "population already has queues");
        }

        auto pair = std::make_unique<QueuePair>();
        pair->population_id = population_id;
        pair->out_name = "out:" + population_id;
        pair->in_name = "in:" + population_id;

        QueuePair* raw = pair.get();
        by_out_name_.emplace(raw->out_name, raw);
        by_in_name_.emplace(raw->in_name, raw);
        by_pop_.emplace(population_id, std::move(pair));

        return MakeCreateOk(population_id, raw->out_name, raw->in_name);
    }

    [[nodiscard]] bool Destroy(const std::string& population_id) {
        std::lock_guard lock(mu_);
        const auto it = by_pop_.find(population_id);
        if (it == by_pop_.end()) {
            return false;
        }
        by_out_name_.erase(it->second->out_name);
        by_in_name_.erase(it->second->in_name);
        by_pop_.erase(it);
        return true;
    }

    [[nodiscard]] QueuePair* FindByPopulation(const std::string& population_id) {
        std::lock_guard lock(mu_);
        const auto it = by_pop_.find(population_id);
        return it == by_pop_.end() ? nullptr : it->second.get();
    }

    [[nodiscard]] PopulationQueue* FindOut(const std::string& out_name) {
        std::lock_guard lock(mu_);
        const auto it = by_out_name_.find(out_name);
        return it == by_out_name_.end() ? nullptr : &it->second->out;
    }

    [[nodiscard]] PopulationQueue* FindIn(const std::string& in_name) {
        std::lock_guard lock(mu_);
        const auto it = by_in_name_.find(in_name);
        return it == by_in_name_.end() ? nullptr : &it->second->in;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mu_);
        return by_pop_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<QueuePair>> by_pop_;
    std::unordered_map<std::string, QueuePair*> by_out_name_;
    std::unordered_map<std::string, QueuePair*> by_in_name_;
};

}  // namespace transport
