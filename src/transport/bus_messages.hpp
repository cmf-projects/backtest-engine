#pragma once

// Logical messages on the population SPSC pair (not UDS).
// docs/POPULATION_TZ.md §5.2–5.3

#include "transport/control.hpp"

#include <cstdint>
#include <string>

namespace transport {

enum class BusMsgType : std::uint32_t {
    Unspecified = 0, // idle / invalid; never on the wire as a real message
    Open = 1,        // engine → venue (in)
    OpenAck = 2,     // venue → engine (out)
    Tick = 3,        // venue → engine (out) — body grows later
    OrderBatch = 4,  // engine → venue (in) — stub for now
    OrdersAck = 5,   // venue → engine (out)
    Advance = 6,     // engine → venue (in)
    End = 7,         // venue → engine (out)
    Error = 8,       // venue → engine (out) fatal
    Close = 9,       // engine → venue (in)
};

// Engine → venue: open population on already-created queues.
struct BusOpen {
    std::string population_id;
};

struct BusOpenAck {
    std::string population_id;
    AckStatus status = AckStatus::Error;
    ErrorCode reason_code = ErrorCode::Unspecified;
    std::string message;
};

// Minimal Tick for bus wiring; books/trades come in a later step.
struct BusTick {
    std::uint64_t pop_ts_ns = 0;
};

struct BusAdvance {};

struct BusEnd {};

struct BusClose {};  // empty; population is implied by the SPSC pair

struct BusError {
    ErrorCode code = ErrorCode::Unspecified;
    std::string message;
};

[[nodiscard]] inline BusOpenAck MakeBusOpenOk(std::string population_id) {
    BusOpenAck ack;
    ack.population_id = std::move(population_id);
    ack.status = AckStatus::Ok;
    return ack;
}

[[nodiscard]] inline BusOpenAck MakeBusOpenReject(std::string population_id, ErrorCode code,
                                                  std::string message) {
    BusOpenAck ack;
    ack.population_id = std::move(population_id);
    ack.status = AckStatus::Error;
    ack.reason_code = code;
    ack.message = std::move(message);
    return ack;
}

}  // namespace transport
