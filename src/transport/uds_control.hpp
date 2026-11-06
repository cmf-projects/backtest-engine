#pragma once

// Unix-socket control channel: listen / connect / CreateQueues handshake.
// Queue names in CreateAck are placeholders until SPSC shm is wired.

#include "transport/control.hpp"

#include <string>

namespace transport {

class UdsListener {
public:
    UdsListener() = default;
    ~UdsListener();

    UdsListener(const UdsListener&) = delete;
    UdsListener& operator=(const UdsListener&) = delete;

    UdsListener(UdsListener&& other) noexcept;
    UdsListener& operator=(UdsListener&& other) noexcept;

    // Bind + listen on path (unlinks stale socket file first).
    [[nodiscard]] static UdsListener Bind(std::string path);

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] int fd() const noexcept { return fd_; }

    // Blocking accept. Returns connected fd, or -1 on error.
    [[nodiscard]] int Accept() const;

private:
    explicit UdsListener(int fd, std::string path);

    int fd_ = -1;
    std::string path_;
};

[[nodiscard]] int UdsConnect(const std::string& path);

// Venue side: read CreateQueues, reply CreateAck (stub queue names on ok).
[[nodiscard]] CreateAck ServeCreateQueues(int conn_fd);

// Engine side: send CreateQueues, wait CreateAck.
[[nodiscard]] CreateAck RequestCreateQueues(int conn_fd, const CreateQueues& req);

}  // namespace transport
