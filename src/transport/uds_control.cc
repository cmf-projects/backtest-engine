#include "transport/uds_control.hpp"

#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/spsc_bus.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace transport {
namespace {

void CloseFd(int fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
    }
}

[[nodiscard]] bool WriteAll(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t off = 0;
    while (off < size) {
        const ssize_t n = ::send(fd, data + off, size - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

[[nodiscard]] bool ReadSome(int fd, std::vector<std::uint8_t>& buf) {
    std::uint8_t tmp[4096];
    const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n < 0) {
        if (errno == EINTR) {
            return ReadSome(fd, buf);
        }
        return false;
    }
    if (n == 0) {
        return false;
    }
    buf.insert(buf.end(), tmp, tmp + n);
    return true;
}

}  // namespace

UdsListener::UdsListener(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}

UdsListener::~UdsListener() {
    CloseFd(fd_);
    fd_ = -1;
    if (!path_.empty()) {
        ::unlink(path_.c_str());
        path_.clear();
    }
}

UdsListener::UdsListener(UdsListener&& other) noexcept
    : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

UdsListener& UdsListener::operator=(UdsListener&& other) noexcept {
    if (this != &other) {
        CloseFd(fd_);
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

UdsListener UdsListener::Bind(std::string path) {
    ::unlink(path.c_str());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("uds: socket() failed");
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        CloseFd(fd);
        throw std::runtime_error("uds: path too long");
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseFd(fd);
        throw std::runtime_error("uds: bind() failed");
    }
    if (::listen(fd, 16) != 0) {
        CloseFd(fd);
        ::unlink(path.c_str());
        throw std::runtime_error("uds: listen() failed");
    }
    return UdsListener(fd, std::move(path));
}

int UdsListener::Accept() const {
    if (fd_ < 0) {
        return -1;
    }
    return ::accept(fd_, nullptr, nullptr);
}

int UdsConnect(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        CloseFd(fd);
        return -1;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseFd(fd);
        return -1;
    }
    return fd;
}

UdsConn::UdsConn(int fd) noexcept : fd_(fd) {}

UdsConn::~UdsConn() {
    CloseFd(fd_);
    fd_ = -1;
}

bool UdsConn::WriteFrame(ControlMsgType type, const std::vector<std::uint8_t>& payload) {
    if (fd_ < 0) {
        return false;
    }
    const auto bytes = Frame(type, payload);
    std::lock_guard<std::mutex> lock(write_mu_);
    return WriteAll(fd_, bytes.data(), bytes.size());
}

bool UdsConn::ReadFrame(ParsedFrame& frame) {
    if (fd_ < 0) {
        return false;
    }
    while (true) {
        std::size_t consumed = 0;
        if (TryParseFrame(inbuf_.data(), inbuf_.size(), frame, &consumed)) {
            inbuf_.erase(inbuf_.begin(),
                         inbuf_.begin() + static_cast<std::ptrdiff_t>(consumed));
            return true;
        }
        if (!ReadSome(fd_, inbuf_)) {
            return false;
        }
    }
}

int UdsConn::ReadFrameDeadline(ParsedFrame& frame,
                               std::chrono::steady_clock::time_point deadline) {
    if (fd_ < 0) {
        return -1;
    }
    while (true) {
        std::size_t consumed = 0;
        if (TryParseFrame(inbuf_.data(), inbuf_.size(), frame, &consumed)) {
            inbuf_.erase(inbuf_.begin(),
                         inbuf_.begin() + static_cast<std::ptrdiff_t>(consumed));
            return 1;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return 0;
        }
        const auto remain_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, static_cast<int>(remain_ms.count()));
        if (pr == 0) {
            return 0;
        }
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (!ReadSome(fd_, inbuf_)) {
            return -1;
        }
    }
}

CreateAck ServeCreateQueues(UdsConn& conn, QueueRegistry& registry) {
    auto reply = [&](CreateAck ack) {
        (void)conn.WriteFrame(ControlMsgType::CreateAck, EncodeCreateAck(ack));
        return ack;
    };

    ParsedFrame frame;
    if (!conn.ReadFrame(frame) || frame.type != ControlMsgType::CreateQueues) {
        return reply(MakeCreateReject("", ErrorCode::CreateFailed, "bad create frame"));
    }

    CreateQueues req;
    if (!DecodeCreateQueues(frame.payload.data(), frame.payload.size(), req)) {
        return reply(MakeCreateReject("", ErrorCode::CreateFailed, "bad create payload"));
    }
    return reply(registry.Create(req.population_id));
}

CreateAck RequestCreateQueues(UdsConn& conn, const CreateQueues& req) {
    if (!conn.WriteFrame(ControlMsgType::CreateQueues, EncodeCreateQueues(req))) {
        return MakeCreateReject(req.population_id, ErrorCode::CreateFailed, "write CreateQueues failed");
    }

    ParsedFrame frame;
    if (!conn.ReadFrame(frame) || frame.type != ControlMsgType::CreateAck) {
        return MakeCreateReject(req.population_id, ErrorCode::CreateFailed, "bad CreateAck frame");
    }

    CreateAck ack;
    if (!DecodeCreateAck(frame.payload.data(), frame.payload.size(), ack)) {
        return MakeCreateReject(req.population_id, ErrorCode::CreateFailed, "bad CreateAck payload");
    }
    return ack;
}

FinishSessionStatus FinishSession(UdsConn& conn, QueueRegistry& registry, const SessionEnded& ended,
                                  std::uint32_t drain_timeout_ms, bool close_already_seen) {
    if (!conn.WriteFrame(ControlMsgType::SessionEnded, EncodeSessionEnded(ended))) {
        (void)registry.Destroy(ended.population_id);
        return FinishSessionStatus::Failed;
    }

    if (close_already_seen) {
        (void)registry.Destroy(ended.population_id);
        return FinishSessionStatus::ClosedByClient;
    }

    QueuePair* pair = registry.FindByPopulation(ended.population_id);
    if (pair == nullptr) {
        (void)registry.Destroy(ended.population_id);
        return FinishSessionStatus::Failed;
    }

    SpscBus in_bus(&pair->in);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        switch (in_bus.TryRecv(type, payload)) {
            case BusRecvResult::Empty:
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                break;
            case BusRecvResult::ProtocolError:
                // Keep draining; corrupt fragments must not block Close.
                break;
            case BusRecvResult::Message:
                if (type == BusMsgType::Close &&
                    DecodeBusClose(payload.data(), payload.size())) {
                    (void)registry.Destroy(ended.population_id);
                    return FinishSessionStatus::ClosedByClient;
                }
                // Discard anything else so Engine can push Close into a free ring.
                break;
        }
    }

    (void)registry.Destroy(ended.population_id);
    return FinishSessionStatus::TimedOut;
}

bool WaitSessionEnded(UdsConn& conn, SessionEnded& out) {
    ParsedFrame frame;
    if (!conn.ReadFrame(frame) || frame.type != ControlMsgType::SessionEnded) {
        return false;
    }
    return DecodeSessionEnded(frame.payload.data(), frame.payload.size(), out);
}

}  // namespace transport
