#pragma once

// Per-population SPSC pairs in POSIX shm (venue process owns Create/Unlink).
// CreateAck.out_queue / in_queue are shm names; engine AttachShmQueue's them.

#include "transport/control.hpp"
#include "transport/shm_queue.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace transport {

struct QueuePair {
    std::string population_id;
    std::string out_name;  // venue → engine (shm)
    std::string in_name;   // engine → venue (shm)
    ShmQueueMapping out;
    ShmQueueMapping in;
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

        const std::string out_name = MakeOutQueueName(population_id);
        const std::string in_name = MakeInQueueName(population_id);

        auto out = CreateShmQueue(out_name);
        if (!out) {
            return MakeCreateReject(population_id, ErrorCode::CreateFailed,
                                    "shm create out failed");
        }
        auto in = CreateShmQueue(in_name);
        if (!in) {
            out->Reset();
            return MakeCreateReject(population_id, ErrorCode::CreateFailed,
                                    "shm create in failed");
        }

        auto pair = std::make_unique<QueuePair>();
        pair->population_id = population_id;
        pair->out_name = out_name;
        pair->in_name = in_name;
        pair->out = std::move(*out);
        pair->in = std::move(*in);

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
        by_pop_.erase(it);  // ShmQueueMapping dtor unmaps + unlinks
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
        return it == by_out_name_.end() ? nullptr : it->second->out.get();
    }

    [[nodiscard]] PopulationQueue* FindIn(const std::string& in_name) {
        std::lock_guard lock(mu_);
        const auto it = by_in_name_.find(in_name);
        return it == by_in_name_.end() ? nullptr : it->second->in.get();
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
