#pragma once

// Unix-socket control channel: listen / connect / CreateQueues / SessionEnded.

#include "transport/bus_messages.hpp"
#include "transport/control.hpp"
#include "transport/queue_registry.hpp"
#include "transport/uds_framing.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace transport {

class UdsListener {
public:
  UdsListener() = default;
  ~UdsListener();

  UdsListener(const UdsListener &) = delete;
  UdsListener &operator=(const UdsListener &) = delete;

  UdsListener(UdsListener &&other) noexcept;
  UdsListener &operator=(UdsListener &&other) noexcept;

  [[nodiscard]] static UdsListener Bind(std::string path);

  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] const std::string &path() const noexcept { return path_; }
  [[nodiscard]] int fd() const noexcept { return fd_; }

  [[nodiscard]] int Accept() const;

private:
  explicit UdsListener(int fd, std::string path);

  int fd_ = -1;
  std::string path_;
};

[[nodiscard]] int UdsConnect(const std::string &path);

// Connected UDS with a sticky read buffer (frames may coalesce in one recv).
// WriteFrame is serialized via write_mu_ so many session threads may share one conn.
// Non-movable: std::mutex member; pass by reference after construction.
class UdsConn {
public:
  explicit UdsConn(int fd) noexcept;
  ~UdsConn();

  UdsConn(const UdsConn &) = delete;
  UdsConn &operator=(const UdsConn &) = delete;
  UdsConn(UdsConn &&) = delete;
  UdsConn &operator=(UdsConn &&) = delete;

  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int fd() const noexcept { return fd_; }

  [[nodiscard]] bool WriteFrame(ControlMsgType type,
                                const std::vector<std::uint8_t> &payload);
  [[nodiscard]] bool ReadFrame(ParsedFrame &frame);
  // 1 = ok, 0 = timeout, -1 = error/EOF
  [[nodiscard]] int
  ReadFrameDeadline(ParsedFrame &frame,
                    std::chrono::steady_clock::time_point deadline);

private:
  int fd_ = -1;
  std::vector<std::uint8_t> inbuf_;
  std::mutex write_mu_;
};

[[nodiscard]] CreateAck ServeCreateQueues(UdsConn &conn, QueueRegistry &registry);
[[nodiscard]] CreateAck RequestCreateQueues(UdsConn &conn,
                                            const CreateQueues &req);

enum class FinishSessionStatus : std::uint8_t {
  Unspecified = 0,
  ClosedByClient = 1,
  TimedOut = 2,
  Failed = 3,
};

// Send SessionEnded on UDS, then drain SPSC in until BusClose | timeout, then
// Destroy. If close_already_seen (engine Close arrived before SessionEnded),
// destroy immediately after SessionEnded — do not wait for a second Close.
[[nodiscard]] FinishSessionStatus
FinishSession(UdsConn &conn, QueueRegistry &registry, const SessionEnded &ended,
              std::uint32_t drain_timeout_ms = kDefaultDrainTimeoutMs,
              bool close_already_seen = false);

[[nodiscard]] bool WaitSessionEnded(UdsConn &conn, SessionEnded &out);

} // namespace transport
