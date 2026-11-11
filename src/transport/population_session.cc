#include "transport/population_session.hpp"

#include "dataset/jsonl_tape.hpp"
#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/bus_population_map.hpp"
#include "transport/log.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace transport {
namespace {

bool WaitProgress(SpscBus& in, BusMsgType& type, std::vector<std::uint8_t>& payload,
                  std::chrono::steady_clock::time_point deadline, BusRecvResult& result,
                  std::atomic<bool>* shutdown) {
    while (std::chrono::steady_clock::now() < deadline) {
        if (shutdown != nullptr && shutdown->load(std::memory_order_relaxed)) {
            result = BusRecvResult::Empty;
            return false;
        }
        result = in.TryRecv(type, payload);
        if (result != BusRecvResult::Empty) {
            return true;
        }
        std::this_thread::yield();
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
    out_ = SpscBus(pair->out.get());
    in_ = SpscBus(pair->in.get());
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

population::TapePtr PopulationSession::MakeTape(const BusComponent& component) const {
    if (cfg_.make_tape) {
        return cfg_.make_tape(component);
    }
    return dataset::MakeJsonlTape(component.dataset_path, component.sec_code);
}

FinishSessionStatus PopulationSession::FinishOk(ErrorCode reason, std::string message,
                                                bool close_already_seen) {
    phase_ = SessionPhase::Finished;
    pop_.reset();
    return FinishSession(conn_, registry_,
                         MakeSessionEndedOk(cfg_.population_id, reason, std::move(message)),
                         cfg_.drain_timeout_ms, close_already_seen);
}

FinishSessionStatus PopulationSession::FinishFatal(ErrorCode reason, std::string message,
                                                   bool close_already_seen) {
    phase_ = SessionPhase::Finished;
    EncodeBusErrorInto(BusError{.code = reason, .message = message}, encode_buf_);
    (void)SendOut(BusMsgType::Error, encode_buf_);
    pop_.reset();
    return FinishSession(conn_, registry_,
                         MakeSessionEndedFatal(cfg_.population_id, reason, std::move(message)),
                         cfg_.drain_timeout_ms, close_already_seen);
}

PopulationSession::Step PopulationSession::SendTickFromPopulation() {
    if (pop_ == nullptr) {
        return FinishFatal(ErrorCode::Internal, "no population", false);
    }
    ToBusTickInto(pop_->MakeTick(), tick_buf_);
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
    for (const auto& slot : tick_buf_.slots) {
        for (const auto& tr : slot.trades) {
            LOG_TRACE("fill pop_ts={} slot={} oid={} comp={} px={} qty={} leaves={} exec={}",
                      tick_buf_.pop_ts_ns, slot.slot_id, tr.client_order_id, tr.component_id,
                      tr.price, tr.qty, tr.leaves, static_cast<unsigned>(tr.exec_type));
        }
    }
    LOG_TRACE("tick pop_ts={} slots={} books_shared={}", tick_buf_.pop_ts_ns, tick_buf_.slots.size(),
              tick_buf_.books.size());
#endif
    EncodeBusTickInto(tick_buf_, encode_buf_);
    if (!SendOut(BusMsgType::Tick, encode_buf_)) {
        return FinishFatal(ErrorCode::Internal, "Tick send failed", false);
    }
    phase_ = SessionPhase::AwaitOrderOrAdvance;
    return std::nullopt;
}

PopulationSession::Step PopulationSession::OnOpen(const std::vector<std::uint8_t>& payload) {
    BusOpen open;
    std::string why;
    if (!DecodeBusOpen(payload.data(), payload.size(), open) ||
        open.population_id != cfg_.population_id || !ValidateBusOpen(open, why)) {
        EncodeBusOpenAckInto(MakeBusOpenReject(cfg_.population_id, ErrorCode::Internal,
                                               why.empty() ? "bad Open" : why),
                             encode_buf_);
        (void)SendOut(BusMsgType::OpenAck, encode_buf_);
        return FinishFatal(ErrorCode::Internal, why.empty() ? "bad Open" : why, false);
    }
    try {
        std::vector<population::ComponentSpec> specs;
        specs.reserve(open.components.size());
        for (const BusComponent& component : open.components) {
            double tick_size = 0.0;
            if (!ParseDecimal(component.tick_size, tick_size) || !(tick_size > 0.0)) {
                throw std::runtime_error("bad tick_size");
            }
            auto tape = MakeTape(component);
            if (tape == nullptr) {
                throw std::runtime_error("make_tape returned null");
            }
            specs.push_back(population::ComponentSpec{.component_id = component.component_id,
                                                      .tick_size = tick_size,
                                                      .tape = std::move(tape)});
        }
        pop_ = std::make_unique<population::Population>(std::move(specs),
                                                        ToSlotConfigs(open.slots));
        pop_->Start();
    } catch (const std::exception& ex) {
        const std::string msg = ex.what();
        EncodeBusOpenAckInto(
            MakeBusOpenReject(cfg_.population_id, ErrorCode::Internal, msg), encode_buf_);
        (void)SendOut(BusMsgType::OpenAck, encode_buf_);
        return FinishFatal(ErrorCode::Internal, msg, false);
    }

    const auto n_slots = static_cast<std::uint32_t>(open.slots.size());
    EncodeBusOpenAckInto(MakeBusOpenOk(cfg_.population_id, n_slots), encode_buf_);
    if (!SendOut(BusMsgType::OpenAck, encode_buf_)) {
        return FinishFatal(ErrorCode::Internal, "OpenAck send failed", false);
    }
    return SendTickFromPopulation();
}

PopulationSession::Step PopulationSession::OnOrders(const std::vector<std::uint8_t>& payload) {
    if (!DecodeBusOrderBatch(payload.data(), payload.size(), order_batch_buf_)) {
        return FinishFatal(ErrorCode::Internal, "bad OrderBatch", false);
    }
    if (pop_ == nullptr) {
        return FinishFatal(ErrorCode::Internal, "no population", false);
    }

    ToOrderBatchInto(order_batch_buf_, domain_batch_);
    const population::OrderBatchResult& result = pop_->Submit(domain_batch_);
    if (result.HasFailure()) {
        EncodeBusOrdersAckInto(MakeOrdersAckReject(ErrorCode::Internal, "bad orders"),
                               encode_buf_);
        (void)SendOut(BusMsgType::OrdersAck, encode_buf_);
        // P0: broken batch is fatal (recoverable OrdersAck later).
        return FinishFatal(ErrorCode::Internal, "OrderBatch rejected", false);
    }
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
    for (const auto& o : order_batch_buf_.orders) {
        LOG_TRACE("submit oid={} slot={} comp={} side={} px={} qty={} tif={}", o.client_order_id,
                  o.slot_id, o.component_id, static_cast<unsigned>(o.side), o.price, o.qty,
                  static_cast<unsigned>(o.tif));
    }
    LOG_TRACE("submit ok orders={} cancels={}", order_batch_buf_.orders.size(),
              order_batch_buf_.cancels.size());
#endif
    EncodeBusOrdersAckInto(MakeOrdersAckOk(), encode_buf_);
    if (!SendOut(BusMsgType::OrdersAck, encode_buf_)) {
        return FinishFatal(ErrorCode::Internal, "OrdersAck send failed", false);
    }
    phase_ = SessionPhase::AwaitAdvance;
    return std::nullopt;
}

PopulationSession::Step PopulationSession::OnAdvance(const std::vector<std::uint8_t>& payload) {
    if (!DecodeBusAdvance(payload.data(), payload.size())) {
        return FinishFatal(ErrorCode::Internal, "bad Advance", false);
    }
    if (pop_ == nullptr) {
        return FinishFatal(ErrorCode::Internal, "no population", false);
    }
    if (!pop_->Advance()) {
        EncodeBusEndInto(encode_buf_);
        if (!SendOut(BusMsgType::End, encode_buf_)) {
            return FinishFatal(ErrorCode::Internal, "End send failed", false);
        }
        return FinishOk(ErrorCode::Unspecified, "tape end", false);
    }
    return SendTickFromPopulation();
}

FinishSessionStatus PopulationSession::Run() {
    if (!valid() || cfg_.population_id.empty()) {
        return FinishSessionStatus::Failed;
    }

    for (;;) {
        if (cfg_.shutdown != nullptr && cfg_.shutdown->load(std::memory_order_relaxed)) {
            return FinishFatal(ErrorCode::Internal, "venue shutdown", false);
        }
        BusMsgType type{};
        BusRecvResult got = BusRecvResult::Empty;
        if (!WaitProgress(in_, type, recv_buf_, StallDeadline(), got, cfg_.shutdown)) {
            if (cfg_.shutdown != nullptr && cfg_.shutdown->load(std::memory_order_relaxed)) {
                return FinishFatal(ErrorCode::Internal, "venue shutdown", false);
            }
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
                done = OnOpen(recv_buf_);
                break;
            case SessionPhase::AwaitOrderOrAdvance:
                done = (type == BusMsgType::OrderBatch) ? OnOrders(recv_buf_) : OnAdvance(recv_buf_);
                break;
            case SessionPhase::AwaitAdvance:
                done = OnAdvance(recv_buf_);
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
