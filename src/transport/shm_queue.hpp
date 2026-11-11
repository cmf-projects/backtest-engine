#pragma once

// POSIX shm-backed PopulationQueue (cross-process SPSC).
// CreateAck.out_queue / in_queue are shm object names (/cmf-…-out|in).

#include "transport/queue_types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace transport {

// Sanitize population_id → POSIX shm-safe suffix ([A-Za-z0-9_-]).
[[nodiscard]] std::string SanitizeShmToken(std::string_view population_id);

[[nodiscard]] std::string MakeOutQueueName(std::string_view population_id);
[[nodiscard]] std::string MakeInQueueName(std::string_view population_id);

// RAII mapping of one PopulationQueue region.
// Owner (venue Create): constructs ring + shm_unlink on destroy.
// Attacher (engine): maps existing; does not unlink.
class ShmQueueMapping {
public:
    ShmQueueMapping() = default;
    ~ShmQueueMapping();

    ShmQueueMapping(const ShmQueueMapping&) = delete;
    ShmQueueMapping& operator=(const ShmQueueMapping&) = delete;

    ShmQueueMapping(ShmQueueMapping&& other) noexcept;
    ShmQueueMapping& operator=(ShmQueueMapping&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] bool owner() const noexcept { return owner_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] PopulationQueue* get() const noexcept { return ptr_; }
    [[nodiscard]] PopulationQueue& operator*() const { return *ptr_; }
    [[nodiscard]] PopulationQueue* operator->() const noexcept { return ptr_; }

    void Reset() noexcept;

private:
    friend std::optional<ShmQueueMapping> CreateShmQueue(const std::string& name);
    friend std::optional<ShmQueueMapping> AttachShmQueue(const std::string& name);

    std::string name_;
    int fd_ = -1;
    PopulationQueue* ptr_ = nullptr;
    bool owner_ = false;
};

// Venue: O_CREAT|O_EXCL (stale name unlinked once), placement-new ring.
[[nodiscard]] std::optional<ShmQueueMapping> CreateShmQueue(const std::string& name);

// Engine: map existing ring (no construct, no unlink).
[[nodiscard]] std::optional<ShmQueueMapping> AttachShmQueue(const std::string& name);

}  // namespace transport
