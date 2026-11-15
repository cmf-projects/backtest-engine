#include "transport/shm_queue.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <new>
#include <string>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace transport {

void FutexWake(std::uint32_t* addr) noexcept {
    ::syscall(SYS_futex, addr, FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

void FutexWait(std::uint32_t* addr, std::uint32_t expected,
               std::chrono::steady_clock::time_point deadline) noexcept {
    const auto remain = deadline - std::chrono::steady_clock::now();
    if (remain <= std::chrono::steady_clock::duration::zero()) {
        return;
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remain).count();
    timespec ts;
    ts.tv_sec = static_cast<time_t>(ns / 1'000'000'000LL);
    ts.tv_nsec = static_cast<long>(ns % 1'000'000'000LL);
    ::syscall(SYS_futex, addr, FUTEX_WAIT, expected, &ts, nullptr, 0);
}

namespace {

constexpr std::size_t kRingBytes = sizeof(PopulationQueue);

[[nodiscard]] bool IsShmSafe(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

[[nodiscard]] bool AllDigits(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// File name cmf-<pid>-<n>-out|in. Returns false when the shape does not match.
[[nodiscard]] bool ParseCmfShmFile(std::string_view file, pid_t& pid) {
    constexpr std::string_view kPrefix = "cmf-";
    if (!file.starts_with(kPrefix)) {
        return false;
    }
    const auto rest = file.substr(kPrefix.size());
    const auto pid_end = rest.find('-');
    if (pid_end == std::string_view::npos || pid_end == 0) {
        return false;
    }
    const auto pid_text = rest.substr(0, pid_end);
    const auto after_pid = rest.substr(pid_end + 1);
    const auto seq_end = after_pid.find('-');
    if (seq_end == std::string_view::npos || seq_end == 0) {
        return false;
    }
    const auto seq_text = after_pid.substr(0, seq_end);
    const auto side = after_pid.substr(seq_end + 1);
    if ((side != "out" && side != "in") || !AllDigits(pid_text) || !AllDigits(seq_text)) {
        return false;
    }
    pid_t parsed = 0;
    const auto* begin = pid_text.data();
    const auto* end = begin + pid_text.size();
    if (std::from_chars(begin, end, parsed).ec != std::errc{} || parsed <= 0) {
        return false;
    }
    pid = parsed;
    return true;
}

void CloseFd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

}  // namespace

bool IsValidPopulationId(std::string_view population_id) {
    if (population_id.empty()) {
        return false;
    }
    for (char c : population_id) {
        if (!IsShmSafe(c)) {
            return false;
        }
    }
    return true;
}

ShmQueueNames MakeUniqueQueueNames() {
    static std::atomic<std::uint64_t> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    const std::string base =
        "/cmf-" + std::to_string(::getpid()) + "-" + std::to_string(n);
    return ShmQueueNames{.out_name = base + "-out", .in_name = base + "-in"};
}

void ReapStaleShmQueues(bool include_self) {
    DIR* dir = ::opendir("/dev/shm");
    if (dir == nullptr) {
        return;
    }
    const pid_t self = ::getpid();
    while (const dirent* ent = ::readdir(dir)) {
        pid_t pid = 0;
        if (!ParseCmfShmFile(ent->d_name, pid)) {
            continue;
        }
        const bool own = pid == self;
        if (own && !include_self) {
            continue;
        }
        if (!own && ::kill(pid, 0) == 0) {
            continue;
        }
        if (!own && errno != ESRCH) {
            continue;
        }
        const std::string shm_name = std::string("/") + ent->d_name;
        ::shm_unlink(shm_name.c_str());
    }
    ::closedir(dir);
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

    // A name already taken belongs to someone else. Do not unlink it.
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
