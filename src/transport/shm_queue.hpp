#pragma once

// POSIX shm-backed PopulationQueue (cross-process SPSC).
// CreateAck.out_queue / in_queue are shm object names (/cmf-<pid>-<n>-out|in).

#include "transport/queue_types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace transport {

// population_id may contain only [A-Za-z0-9_-]. Empty is invalid.
[[nodiscard]] bool IsValidPopulationId(std::string_view population_id);

struct ShmQueueNames {
    std::string out_name;
    std::string in_name;
};

// Fresh names for this process. Two calls never share a name.
[[nodiscard]] ShmQueueNames MakeUniqueQueueNames();

// Drop cmf-<pid>-* objects whose pid is dead. When include_self is set, also
// drop this pid's objects — only safe before this process has created any.
void ReapStaleShmQueues(bool include_self);

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

// Venue: O_CREAT|O_EXCL, placement-new ring. Does not unlink an existing name.
[[nodiscard]] std::optional<ShmQueueMapping> CreateShmQueue(const std::string& name);

// Engine: map existing ring (no construct, no unlink).
[[nodiscard]] std::optional<ShmQueueMapping> AttachShmQueue(const std::string& name);

}  // namespace transport
