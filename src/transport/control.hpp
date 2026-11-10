#pragma once

// UDS control-plane types (create queues / SessionEnded).
// Order/tick wire codes live elsewhere when SPSC framing appears.

#include <cstdint>
#include <string>

namespace transport {

// Venue config default until a real config object exists.
inline constexpr std::uint32_t kDefaultDrainTimeoutMs = 60'000;

// Only codes used on the UDS control path for now.
enum class ErrorCode : std::uint32_t {
    Unspecified = 0,
    Internal = 7,
    ClientClose = 14,
    DrainTimeout = 15,
    NoQuota = 16,
    CreateFailed = 17,
};

enum class AckStatus : std::uint8_t {
    Unspecified = 0,  // invalid; treat as Error
    Ok = 1,
    Error = 2,
};

enum class SessionEndStatus : std::uint8_t {
    Ok = 1,
    Fatal = 2,
};

// Engine → venue: allocate SPSC pair for this population.
struct CreateQueues {
    std::string population_id;
};

// Venue → Engine after CreateQueues.
struct CreateAck {
    std::string population_id;
    AckStatus status = AckStatus::Error;
    ErrorCode reason_code = ErrorCode::Unspecified;  // set when status == Error
    std::string message;
    std::string out_queue;  // venue → engine; set when Ok
    std::string in_queue;   // engine → venue; set when Ok
};

// Venue → Engine after the run (ok or fatal).
struct SessionEnded {
    std::string population_id;
    SessionEndStatus status = SessionEndStatus::Fatal;
    ErrorCode reason_code = ErrorCode::Unspecified;
    std::string message;
};

[[nodiscard]] inline CreateAck MakeCreateOk(std::string population_id, std::string out_queue,
                                            std::string in_queue) {
    CreateAck ack;
    ack.population_id = std::move(population_id);
    ack.status = AckStatus::Ok;
    ack.out_queue = std::move(out_queue);
    ack.in_queue = std::move(in_queue);
    return ack;
}

[[nodiscard]] inline CreateAck MakeCreateReject(std::string population_id, ErrorCode code,
                                                std::string message) {
    CreateAck ack;
    ack.population_id = std::move(population_id);
    ack.status = AckStatus::Error;
    ack.reason_code = code;
    ack.message = std::move(message);
    return ack;
}

[[nodiscard]] inline SessionEnded MakeSessionEndedOk(std::string population_id,
                                                     ErrorCode reason = ErrorCode::Unspecified,
                                                     std::string message = {}) {
    SessionEnded ended;
    ended.population_id = std::move(population_id);
    ended.status = SessionEndStatus::Ok;
    ended.reason_code = reason;
    ended.message = std::move(message);
    return ended;
}

[[nodiscard]] inline SessionEnded MakeSessionEndedFatal(std::string population_id, ErrorCode reason,
                                                        std::string message) {
    SessionEnded ended;
    ended.population_id = std::move(population_id);
    ended.status = SessionEndStatus::Fatal;
    ended.reason_code = reason;
    ended.message = std::move(message);
    return ended;
}

}  // namespace transport
