#include "transport/population_session.hpp"

#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"

#include <chrono>
#include <thread>
#include <vector>

namespace transport {
namespace {

bool WaitProgress(SpscBus& in, BusMsgType& type, std::vector<std::uint8_t>& payload,
                  std::chrono::steady_clock::time_point deadline, BusRecvResult& result) {
    while (std::chrono::steady_clock::now() < deadline) {
        result = in.TryRecv(type, payload);
        if (result != BusRecvResult::Empty) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    result = BusRecvResult::Empty;
    return false;
}

}  // namespace

PopulationSession::PopulationSession(UdsConn& conn, QueueRegistry& registry, SessionConfig cfg)
    : conn_(conn), registry_(registry), cfg_(std::move(cfg)), out_(nullptr), in_(nullptr) {
    QueuePair* pair = registry_.FindByPopulation(cfg_.population_id);
    if (pair == nullptr) {
        return;
    }
    out_ = SpscBus(&pair->out);
    in_ = SpscBus(&pair->in);
}

bool PopulationSession::SendOut(BusMsgType type, const std::vector<std::uint8_t>& payload) {
    return out_.TrySend(type, payload);
}

BusRecvResult PopulationSession::RecvIn(BusMsgType& type, std::vector<std::uint8_t>& payload) {
    return in_.TryRecv(type, payload);
}

std::chrono::steady_clock::time_point PopulationSession::StallDeadline() const {
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.drain_timeout_ms);
}

bool PopulationSession::Allowed(SessionPhase phase, BusMsgType type) noexcept {
    switch (phase) {
        case SessionPhase::AwaitOpen:
            return type == BusMsgType::Open || type == BusMsgType::Close;
        case SessionPhase::AwaitOrderOrAdvance:
            return type == BusMsgType::OrderBatch || type == BusMsgType::Advance ||
                   type == BusMsgType::Close;
        case SessionPhase::AwaitAdvance:
            return type == BusMsgType::Advance || type == BusMsgType::Close;
        case SessionPhase::Finished:
            return false;
    }
    return false;
}

FinishSessionStatus PopulationSession::FinishOk(ErrorCode reason, std::string message,
                                                bool close_already_seen) {
    phase_ = SessionPhase::Finished;
    return FinishSession(conn_, registry_,
                         MakeSessionEndedOk(cfg_.population_id, reason, std::move(message)),
                         cfg_.drain_timeout_ms, close_already_seen);
}

FinishSessionStatus PopulationSession::FinishFatal(ErrorCode reason, std::string message,
                                                   bool close_already_seen) {
    phase_ = SessionPhase::Finished;
    (void)SendOut(BusMsgType::Error,
                  EncodeBusError(BusError{.code = reason, .message = message}));
    return FinishSession(conn_, registry_,
                         MakeSessionEndedFatal(cfg_.population_id, reason, std::move(message)),
                         cfg_.drain_timeout_ms, close_already_seen);
}

PopulationSession::Step PopulationSession::OnOpen(const std::vector<std::uint8_t>& payload) {
    BusOpen open;
    std::string why;
    if (!DecodeBusOpen(payload.data(), payload.size(), open) ||
        open.population_id != cfg_.population_id || !ValidateBusOpen(open, why)) {
        (void)SendOut(BusMsgType::OpenAck,
                      EncodeBusOpenAck(MakeBusOpenReject(cfg_.population_id, ErrorCode::Internal,
                                                         why.empty() ? "bad Open" : why)));
        return FinishFatal(ErrorCode::Internal, why.empty() ? "bad Open" : why, false);
    }
    slots_ = open.slots;
    n_slots_ = static_cast<std::uint32_t>(slots_.size());
    if (!SendOut(BusMsgType::OpenAck,
                 EncodeBusOpenAck(MakeBusOpenOk(cfg_.population_id, n_slots_)))) {
        return FinishFatal(ErrorCode::Internal, "OpenAck send failed", false);
    }
    BusTick tick;
    tick.pop_ts_ns = 1;
    tick.slots.reserve(slots_.size());
    for (const auto& slot : slots_) {
        BusSlotTick row;
        row.slot_id = slot.slot_id;
        tick.slots.push_back(std::move(row));
    }
    if (!SendOut(BusMsgType::Tick, EncodeBusTick(tick))) {
        return FinishFatal(ErrorCode::Internal, "Tick send failed", false);
    }
    ++ticks_sent_;
    phase_ = SessionPhase::AwaitOrderOrAdvance;
    return std::nullopt;
}

PopulationSession::Step PopulationSession::OnOrders(const std::vector<std::uint8_t>& payload) {
    BusOrderBatch batch;
    if (!DecodeBusOrderBatch(payload.data(), payload.size(), batch)) {
        return FinishFatal(ErrorCode::Internal, "bad OrderBatch", false);
    }
    // 2.1: wire round-trip only; Population::Submit lands in 2.2.
    if (!SendOut(BusMsgType::OrdersAck, EncodeBusOrdersAck(MakeOrdersAckOk()))) {
        return FinishFatal(ErrorCode::Internal, "OrdersAck send failed", false);
    }
    phase_ = SessionPhase::AwaitAdvance;
    return std::nullopt;
}

PopulationSession::Step PopulationSession::OnAdvance(const std::vector<std::uint8_t>& payload) {
    if (!DecodeBusAdvance(payload.data(), payload.size())) {
        return FinishFatal(ErrorCode::Internal, "bad Advance", false);
    }
    if (ticks_sent_ >= cfg_.stub_ticks) {
        if (!SendOut(BusMsgType::End, EncodeBusEnd())) {
            return FinishFatal(ErrorCode::Internal, "End send failed", false);
        }
        return FinishOk(ErrorCode::Unspecified, "tape end", false);
    }
    BusTick tick;
    tick.pop_ts_ns = static_cast<std::uint64_t>(ticks_sent_ + 1);
    tick.slots.reserve(slots_.size());
    for (const auto& slot : slots_) {
        BusSlotTick row;
        row.slot_id = slot.slot_id;
        tick.slots.push_back(std::move(row));
    }
    if (!SendOut(BusMsgType::Tick, EncodeBusTick(tick))) {
        return FinishFatal(ErrorCode::Internal, "Tick send failed", false);
    }
    ++ticks_sent_;
    phase_ = SessionPhase::AwaitOrderOrAdvance;
    return std::nullopt;
}

FinishSessionStatus PopulationSession::Run() {
    if (!valid() || cfg_.population_id.empty() || cfg_.stub_ticks == 0) {
        return FinishSessionStatus::Failed;
    }

    for (;;) {
        BusMsgType type{};
        std::vector<std::uint8_t> payload;
        BusRecvResult got = BusRecvResult::Empty;
        if (!WaitProgress(in_, type, payload, StallDeadline(), got)) {
            return FinishFatal(ErrorCode::DrainTimeout, "no progress before drain_timeout", false);
        }
        if (got == BusRecvResult::ProtocolError) {
            return FinishFatal(ErrorCode::Internal, "bus protocol error on in", false);
        }
        if (!Allowed(phase_, type)) {
            return FinishFatal(ErrorCode::Internal, "message not allowed in current phase", false);
        }
        if (type == BusMsgType::Close) {
            const char* msg =
                phase_ == SessionPhase::AwaitOpen ? "close before open" : "client close";
            return FinishOk(ErrorCode::ClientClose, msg, /*close_already_seen=*/true);
        }

        Step done;
        switch (phase_) {
            case SessionPhase::AwaitOpen:
                done = OnOpen(payload);
                break;
            case SessionPhase::AwaitOrderOrAdvance:
                done = (type == BusMsgType::OrderBatch) ? OnOrders(payload) : OnAdvance(payload);
                break;
            case SessionPhase::AwaitAdvance:
                done = OnAdvance(payload);
                break;
            case SessionPhase::Finished:
                return FinishSessionStatus::Failed;
        }
        if (done.has_value()) {
            return *done;
        }
    }
}

}  // namespace transport
