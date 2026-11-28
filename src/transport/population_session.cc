#include "transport/population_session.hpp"

#include "hot_log.hpp"

#include "dataset/make_tape.hpp"
#include "population/price_ticks.hpp"
#include "transport/bus_framing.hpp"
#include "transport/bus_messages.hpp"
#include "transport/bus_population_map.hpp"
#include "transport/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace transport {
namespace {

bool WaitProgress(SpscBus& in, BusMsgType& type, std::vector<std::uint8_t>& payload,
                  std::chrono::steady_clock::time_point deadline, BusRecvResult& result,
                  std::atomic<bool>* shutdown, std::atomic<bool>* conn_stop) {
    while (std::chrono::steady_clock::now() < deadline) {
        const bool stop = (shutdown != nullptr && shutdown->load(std::memory_order_relaxed)) ||
                          (conn_stop != nullptr && conn_stop->load(std::memory_order_relaxed));
        if (stop) {
            result = BusRecvResult::Empty;
            return false;
        }
        result = in.TryRecv(type, payload);
        if (result != BusRecvResult::Empty) {
            return true;
        }
        if ((shutdown != nullptr && shutdown->load(std::memory_order_relaxed)) ||
            (conn_stop != nullptr && conn_stop->load(std::memory_order_relaxed))) {
            result = BusRecvResult::Empty;
            return false;
        }
        in.Wait(deadline);
    }
    result = in.TryRecv(type, payload);
    return result != BusRecvResult::Empty;
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
    return out_.TrySend(type, payload, StallDeadline(), cfg_.shutdown, cfg_.conn_stop);
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
    return dataset::MakeTape(component.dataset_path, component.sec_code);
}

FinishSessionStatus PopulationSession::FinishOk(ErrorCode reason, std::string message,
                                                bool close_already_seen) {
    phase_ = SessionPhase::Finished;
    pop_.reset();
    return FinishSession(conn_, registry_,
                         MakeSessionEndedOk(cfg_.population_id, reason, std::move(message)),
                         cfg_.drain_timeout_ms, close_already_seen, cfg_.shutdown);
}

FinishSessionStatus PopulationSession::FinishFatal(ErrorCode reason, std::string message,
                                                   bool close_already_seen) {
    phase_ = SessionPhase::Finished;
    EncodeBusErrorInto(BusError{.code = reason, .message = message}, encode_buf_);
    // Best-effort. A full ring already burned the drain deadline; do not wait again.
    (void)out_.TrySend(BusMsgType::Error, encode_buf_);
    pop_.reset();
    return FinishSession(conn_, registry_,
                         MakeSessionEndedFatal(cfg_.population_id, reason, std::move(message)),
                         cfg_.drain_timeout_ms, close_already_seen, cfg_.shutdown);
}

PopulationSession::Step PopulationSession::SendTickFromPopulation() {
    if (pop_ == nullptr) {
        return FinishFatal(ErrorCode::Internal, "no population", false);
    }
#if CMF_HOT_LOG
    const auto t0 = std::chrono::steady_clock::now();
#endif
    ToBusTickInto(pop_->MakeTick(), tick_buf_, tick_sizes_);
#if CMF_HOT_LOG
    const auto t1 = std::chrono::steady_clock::now();
#endif
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
    for (const auto& slot : tick_buf_.slots) {
        for (const auto& tr : slot.trades) {
            LOG_TRACE(
                "fill pop_ts={} slot={} oid={} comp={} px={}+{} qty={} leaves={} exec={}",
                tick_buf_.pop_ts_ns, slot.slot_id, tr.client_order_id, tr.component_id,
                tr.price.whole, tr.price.frac, tr.qty, tr.leaves,
                static_cast<unsigned>(tr.exec_type));
        }
    }
    LOG_TRACE("tick pop_ts={} slots={} books_shared={}", tick_buf_.pop_ts_ns, tick_buf_.slots.size(),
              tick_buf_.books.size());
#endif
    EncodeBusTickInto(tick_buf_, encode_buf_);
#if CMF_HOT_LOG
    const auto t2 = std::chrono::steady_clock::now();
#endif
    if (!SendOut(BusMsgType::Tick, encode_buf_)) {
        return FinishFatal(ErrorCode::DrainTimeout, "ring full before drain_timeout", false);
    }
#if CMF_HOT_LOG
    const auto t3 = std::chrono::steady_clock::now();
    static std::uint64_t make_ns = 0;
    static std::uint64_t enc_ns = 0;
    static std::uint64_t send_ns = 0;
    static std::uint64_t n = 0;
    static auto last = t3;
    make_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    enc_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
    send_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count());
    ++n;
    if (t3 - last >= std::chrono::seconds(5)) {
        std::fprintf(stderr, "hot sendtick make_ms=%.0f encode_ms=%.0f send_ms=%.0f n=%llu bytes=%zu\n",
                     make_ns / 1e6, enc_ns / 1e6, send_ns / 1e6, static_cast<unsigned long long>(n),
                     encode_buf_.size());
        make_ns = 0;
        enc_ns = 0;
        send_ns = 0;
        n = 0;
        last = t3;
    }
#endif
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
        tick_sizes_.clear();
        std::vector<population::ComponentSpec> specs;
        specs.reserve(open.components.size());
        for (const BusComponent& component : open.components) {
            double tick_size = 0.0;
            if (!ParseDecimal(component.tick_size, tick_size) || !(tick_size > 0.0)) {
                throw std::runtime_error("bad tick_size");
            }
            if (!population::TickSizeFitsWholeFrac(tick_size)) {
                throw std::runtime_error("tick_size does not divide 1");
            }
            tick_sizes_[component.component_id] = tick_size;
            if (component.sec_code == "*") {
                JournalHead head;
                head.journal = std::make_unique<dataset::FeatherJournal>(component.dataset_path);
                head.tick = tick_size;
                head.ok = head.journal->Next(head.symbol, head.event);
                LOG_INFO("journal component={} path={} first={}", component.component_id,
                         component.dataset_path, head.ok ? head.symbol : "-");
                journals_.push_back(std::move(head));
                continue;
            }
            LOG_INFO("loading dataset component={} path={} symbol={}", component.component_id,
                     component.dataset_path, component.sec_code);
            population::TapePtr tape;
            try {
                tape = MakeTape(component);
            } catch (const std::exception& ex) {
                const std::string msg = ex.what();
                if (msg.find("no rows for symbol") == std::string::npos) {
                    throw;
                }
                LOG_INFO("skip component={} symbol={} no rows", component.component_id,
                         component.sec_code);
                continue;
            }
            if (tape == nullptr) {
                throw std::runtime_error("make_tape returned null");
            }
            LOG_INFO("dataset loaded component={} symbol={} start_ts={}", component.component_id,
                     tape->FirstSymbol(), tape->StartTs());
            specs.push_back(population::ComponentSpec{.component_id = component.component_id,
                                                      .tick_size = tick_size,
                                                      .tape = std::move(tape)});
        }
        if (specs.empty() && journals_.empty()) {
            throw std::runtime_error("no components with rows");
        }
        if (open.step_ns == 0 || open.step_ns < -1) {
            throw std::runtime_error("bad step_ns");
        }
        step_ns_ = open.step_ns;
        if (specs.empty()) {
            std::uint64_t start = 0;
            bool any = false;
            for (const JournalHead& head : journals_) {
                if (!head.ok) {
                    continue;
                }
                if (!any || head.event.ts_ns < start) {
                    start = head.event.ts_ns;
                    any = true;
                }
            }
            if (!any) {
                throw std::runtime_error("journals have no rows");
            }
            pop_ = std::make_unique<population::Population>(ToSlotConfigs(open.slots));
            pop_->SetStepNs(open.step_ns);
            DrainJournals(start);
            pop_->StartAt(start);
        } else {
        pop_ = std::make_unique<population::Population>(std::move(specs),
                                                        ToSlotConfigs(open.slots));
        pop_->SetStepNs(open.step_ns);
        pop_->Start();
        }
        LOG_INFO("population started pop_ts={} components={} step_ns={} delta_ns={}",
                 pop_->clock().pop_ts_ns(), open.components.size(), open.step_ns,
                 open.slots.empty() ? 0 : open.slots.front().delta_ns);
    } catch (const std::exception& ex) {
        LOG_ERROR("open failed: {}", ex.what());
        const std::string msg = ex.what();
        EncodeBusOpenAckInto(
            MakeBusOpenReject(cfg_.population_id, ErrorCode::Internal, msg), encode_buf_);
        (void)SendOut(BusMsgType::OpenAck, encode_buf_);
        return FinishFatal(ErrorCode::Internal, msg, false);
    }

    PrepareTickStorage(tick_buf_, open.slots.size(), open.components.size());
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

    ToOrderBatchInto(order_batch_buf_, domain_batch_, tick_sizes_);
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
        LOG_TRACE("submit oid={} slot={} comp={} side={} px={}+{} qty={} tif={}",
                  o.client_order_id, o.slot_id, o.component_id, static_cast<unsigned>(o.side),
                  o.price.whole, o.price.frac, o.qty, static_cast<unsigned>(o.tif));
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

namespace {

[[nodiscard]] std::string ComponentId(std::string_view symbol) {
    std::string id;
    id.reserve(symbol.size());
    for (const char c : symbol) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok) {
            id.push_back(c);
        } else if (c == '.') {
            id.push_back('p');
        } else if (!id.empty() && id.back() != '_') {
            id.push_back('_');
        }
    }
    while (!id.empty() && id.back() == '_') {
        id.pop_back();
    }
    return id;
}

}  // namespace

void PopulationSession::DrainJournals(std::uint64_t now_ns) {
    for (JournalHead& head : journals_) {
        while (head.ok && head.event.ts_ns <= now_ns) {
            const std::string id = ComponentId(head.symbol);
            tick_sizes_[id] = head.tick;
            if (pop_->Observe(id, head.tick, head.event)) {
#if CMF_HOT_LOG
                std::fprintf(stderr, "instrument arrive %s\n", id.c_str());
#endif
            }
            head.ok = head.journal->Next(head.symbol, head.event);
        }
    }
    const std::size_t before = pop_->component_count();
    pop_->DropExpired(now_ns);
    if (pop_->component_count() != before) {
#if CMF_HOT_LOG
        std::fprintf(stderr, "instrument drop left=%zu\n", pop_->component_count());
#endif
    }
}

bool PopulationSession::AdvanceJournals() {
    std::optional<std::uint64_t> pending;
    for (const JournalHead& head : journals_) {
        if (!head.ok) {
            continue;
        }
        if (!pending.has_value() || head.event.ts_ns < *pending) {
            pending = head.event.ts_ns;
        }
    }
    if (!pending.has_value()) {
        return false;
    }
    const std::uint64_t pop = pop_->clock().pop_ts_ns();
    std::uint64_t next = 0;
    if (*pending > pop && (*pending - pop) > population::kSessionGapNs) {
        next = *pending;
    } else if (step_ns_ == -1) {
        next = *pending;
    } else {
        next = pop + static_cast<std::uint64_t>(step_ns_);
    }
    if (next <= pop) {
        return false;
    }
    DrainJournals(next);
    pop_->AdvanceTo(next);
    return true;
}

PopulationSession::Step PopulationSession::OnAdvance(const std::vector<std::uint8_t>& payload) {
    if (!DecodeBusAdvance(payload.data(), payload.size())) {
        return FinishFatal(ErrorCode::Internal, "bad Advance", false);
    }
    if (pop_ == nullptr) {
        return FinishFatal(ErrorCode::Internal, "no population", false);
    }
#if CMF_HOT_LOG
    const auto t0 = std::chrono::steady_clock::now();
#endif
    const bool stepped = journals_.empty() ? pop_->Advance() : AdvanceJournals();
    if (!stepped) {
        EncodeBusEndInto(encode_buf_);
        if (!SendOut(BusMsgType::End, encode_buf_)) {
            return FinishFatal(ErrorCode::Internal, "End send failed", false);
        }
        return FinishOk(ErrorCode::Unspecified, "tape end", false);
    }
    const Step step = SendTickFromPopulation();
#if CMF_HOT_LOG
    const auto t1 = std::chrono::steady_clock::now();
    static std::uint64_t adv_ns = 0;
    static std::uint64_t n = 0;
    static auto last = t1;
    adv_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    ++n;
    if (t1 - last >= std::chrono::seconds(5)) {
        std::fprintf(stderr, "hot session advance_ms=%.0f n=%llu\n", adv_ns / 1e6,
                     static_cast<unsigned long long>(n));
        adv_ns = 0;
        n = 0;
        last = t1;
    }
#endif
    return step;
}

FinishSessionStatus PopulationSession::Run() {
    if (!valid() || cfg_.population_id.empty()) {
        return FinishSessionStatus::Failed;
    }

    for (;;) {
        if (cfg_.conn_stop != nullptr && cfg_.conn_stop->load(std::memory_order_relaxed)) {
            return FinishFatal(ErrorCode::Internal, "engine disconnected", false);
        }
        if (cfg_.shutdown != nullptr && cfg_.shutdown->load(std::memory_order_relaxed)) {
            return FinishFatal(ErrorCode::Internal, "venue shutdown", false);
        }
        BusMsgType type{};
        BusRecvResult got = BusRecvResult::Empty;
        if (!WaitProgress(in_, type, recv_buf_, StallDeadline(), got, cfg_.shutdown,
                          cfg_.conn_stop)) {
            if (cfg_.conn_stop != nullptr && cfg_.conn_stop->load(std::memory_order_relaxed)) {
                return FinishFatal(ErrorCode::Internal, "engine disconnected", false);
            }
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
        try {
            switch (phase_) {
                case SessionPhase::AwaitOpen:
                    done = OnOpen(recv_buf_);
                    break;
                case SessionPhase::AwaitOrderOrAdvance:
                    done = (type == BusMsgType::OrderBatch) ? OnOrders(recv_buf_)
                                                           : OnAdvance(recv_buf_);
                    break;
                case SessionPhase::AwaitAdvance:
                    done = OnAdvance(recv_buf_);
                    break;
                case SessionPhase::Finished:
                    return FinishSessionStatus::Failed;
            }
        } catch (const std::exception& ex) {
            return FinishFatal(ErrorCode::Internal, ex.what(), false);
        } catch (...) {
            return FinishFatal(ErrorCode::Internal, "unknown exception", false);
        }
        if (done.has_value()) {
            return *done;
        }
    }
}

}  // namespace transport
