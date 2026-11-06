#include "transport/uds_control.hpp"

#include "transport/uds_framing.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

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

[[nodiscard]] bool ReadFrame(int fd, ParsedFrame& frame) {
    std::vector<std::uint8_t> buf;
    while (true) {
        std::size_t consumed = 0;
        if (TryParseFrame(buf.data(), buf.size(), frame, &consumed)) {
            if (consumed < buf.size()) {
                // Leftover bytes are unexpected for this handshake step.
                return false;
            }
            return true;
        }
        if (!ReadSome(fd, buf)) {
            return false;
        }
    }
}

[[nodiscard]] bool WriteFrame(int fd, ControlMsgType type,
                              const std::vector<std::uint8_t>& payload) {
    const auto bytes = Frame(type, payload);
    return WriteAll(fd, bytes.data(), bytes.size());
}

CreateAck ReplyCreateAck(int conn_fd, CreateAck ack) {
    (void)WriteFrame(conn_fd, ControlMsgType::CreateAck, EncodeCreateAck(ack));
    return ack;
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

CreateAck ServeCreateQueues(int conn_fd) {
    ParsedFrame frame;
    if (!ReadFrame(conn_fd, frame) || frame.type != ControlMsgType::CreateQueues) {
        return ReplyCreateAck(conn_fd,
                              MakeCreateReject("", ErrorCode::CreateFailed, "bad create frame"));
    }

    CreateQueues req;
    if (!DecodeCreateQueues(frame.payload.data(), frame.payload.size(), req)) {
        return ReplyCreateAck(conn_fd,
                              MakeCreateReject("", ErrorCode::CreateFailed, "bad create payload"));
    }
    if (req.population_id.empty()) {
        return ReplyCreateAck(conn_fd,
                              MakeCreateReject("", ErrorCode::CreateFailed, "empty population_id"));
    }

    // Placeholder names until real SPSC shm allocation exists.
    return ReplyCreateAck(
        conn_fd,
        MakeCreateOk(req.population_id, "out:" + req.population_id, "in:" + req.population_id));
}

CreateAck RequestCreateQueues(int conn_fd, const CreateQueues& req) {
    if (!WriteFrame(conn_fd, ControlMsgType::CreateQueues, EncodeCreateQueues(req))) {
        return MakeCreateReject(req.population_id, ErrorCode::CreateFailed, "write CreateQueues failed");
    }

    ParsedFrame frame;
    if (!ReadFrame(conn_fd, frame) || frame.type != ControlMsgType::CreateAck) {
        return MakeCreateReject(req.population_id, ErrorCode::CreateFailed, "bad CreateAck frame");
    }

    CreateAck ack;
    if (!DecodeCreateAck(frame.payload.data(), frame.payload.size(), ack)) {
        return MakeCreateReject(req.population_id, ErrorCode::CreateFailed, "bad CreateAck payload");
    }
    return ack;
}

}  // namespace transport
