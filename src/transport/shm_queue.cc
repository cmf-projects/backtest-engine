#include "transport/shm_queue.hpp"

#include <new>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace transport {
namespace {

constexpr std::size_t kRingBytes = sizeof(PopulationQueue);

[[nodiscard]] bool IsShmSafe(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

void CloseFd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

}  // namespace

std::string SanitizeShmToken(std::string_view population_id) {
    std::string out;
    out.reserve(population_id.size());
    for (char c : population_id) {
        out.push_back(IsShmSafe(c) ? c : '_');
    }
    if (out.empty()) {
        out = "pop";
    }
    return out;
}

std::string MakeOutQueueName(std::string_view population_id) {
    return "/cmf-" + SanitizeShmToken(population_id) + "-out";
}

std::string MakeInQueueName(std::string_view population_id) {
    return "/cmf-" + SanitizeShmToken(population_id) + "-in";
}

ShmQueueMapping::~ShmQueueMapping() { Reset(); }

ShmQueueMapping::ShmQueueMapping(ShmQueueMapping&& other) noexcept
    : name_(std::move(other.name_)),
      fd_(other.fd_),
      ptr_(other.ptr_),
      owner_(other.owner_) {
    other.fd_ = -1;
    other.ptr_ = nullptr;
    other.owner_ = false;
}

ShmQueueMapping& ShmQueueMapping::operator=(ShmQueueMapping&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Reset();
    name_ = std::move(other.name_);
    fd_ = other.fd_;
    ptr_ = other.ptr_;
    owner_ = other.owner_;
    other.fd_ = -1;
    other.ptr_ = nullptr;
    other.owner_ = false;
    return *this;
}

void ShmQueueMapping::Reset() noexcept {
    if (ptr_ != nullptr) {
        if (owner_) {
            ptr_->~PopulationQueue();
        }
        ::munmap(ptr_, kRingBytes);
        ptr_ = nullptr;
    }
    CloseFd(fd_);
    if (owner_ && !name_.empty()) {
        ::shm_unlink(name_.c_str());
    }
    owner_ = false;
    name_.clear();
}

std::optional<ShmQueueMapping> CreateShmQueue(const std::string& name) {
    if (name.empty() || name[0] != '/') {
        return std::nullopt;
    }

    // Crash leftovers: drop stale object once so O_EXCL can succeed.
    ::shm_unlink(name.c_str());

    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        return std::nullopt;
    }
    if (::ftruncate(fd, static_cast<off_t>(kRingBytes)) != 0) {
        ::close(fd);
        ::shm_unlink(name.c_str());
        return std::nullopt;
    }

    void* mem = ::mmap(nullptr, kRingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        ::close(fd);
        ::shm_unlink(name.c_str());
        return std::nullopt;
    }

    auto* ring = new (mem) PopulationQueue();

    ShmQueueMapping map;
    map.name_ = name;
    map.fd_ = fd;
    map.ptr_ = ring;
    map.owner_ = true;
    return map;
}

std::optional<ShmQueueMapping> AttachShmQueue(const std::string& name) {
    if (name.empty() || name[0] != '/') {
        return std::nullopt;
    }

    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        return std::nullopt;
    }

    void* mem = ::mmap(nullptr, kRingBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        ::close(fd);
        return std::nullopt;
    }

    ShmQueueMapping map;
    map.name_ = name;
    map.fd_ = fd;
    map.ptr_ = static_cast<PopulationQueue*>(mem);
    map.owner_ = false;
    return map;
}

}  // namespace transport
