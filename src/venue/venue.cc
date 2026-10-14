// ============================================================================
// Venue: book + matching + dataset feed.
//
// Loop shape (do not break):
//     Start -> Ready -> { AdvanceTo, publish, answer } -> Finish
//
// See docs/CONTEXT.md for roles.
// ============================================================================

#include "venue/venue.hpp"

#include "cmf/cmf_driver.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cmf_driver::Level;
using cmf_driver::LevelChange;
using cmf_driver::RejectReason;
using cmf_driver::Session;
using cmf_driver::Side;
using cmf_driver::SimTime;
using cmf_driver::TimeInForce;

struct MarketEvent {
    std::uint64_t seq = 0;
    SimTime ts = 0;
    std::string symbol;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

[[nodiscard]] std::optional<std::string_view> JsonStringField(std::string_view line,
                                                              std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\":\"";
    const auto pos = line.find(pattern);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = pos + pattern.size();
    const auto end = line.find('"', start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return line.substr(start, end - start);
}

[[nodiscard]] std::optional<std::uint64_t> JsonU64Field(std::string_view line,
                                                        std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\":";
    const auto pos = line.find(pattern);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = pos + pattern.size();
    std::uint64_t value = 0;
    const auto* begin = line.data() + start;
    const auto* end = line.data() + line.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr == begin) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::vector<Level> JsonLevels(std::string_view line, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\":[";
    const auto pos = line.find(pattern);
    if (pos == std::string_view::npos) {
        return {};
    }
    auto i = pos + pattern.size();
    std::vector<Level> levels;
    while (i < line.size() && line[i] != ']') {
        while (i < line.size() && (line[i] == ' ' || line[i] == ',')) {
            ++i;
        }
        if (i < line.size() && line[i] == ']') {
            break;
        }
        if (i >= line.size() || line[i] != '{') {
            break;
        }
        const auto obj_end = line.find('}', i);
        if (obj_end == std::string_view::npos) {
            break;
        }
        const auto obj = line.substr(i, obj_end - i + 1);

        Level level;
        const auto px_pos = obj.find("\"price\":");
        const auto qty_pos = obj.find("\"qty\":");
        if (px_pos == std::string_view::npos || qty_pos == std::string_view::npos) {
            break;
        }
        level.price = std::strtod(obj.data() + px_pos + 8, nullptr);
        level.qty = static_cast<std::uint64_t>(std::strtoull(obj.data() + qty_pos + 6, nullptr, 10));
        levels.push_back(level);
        i = obj_end + 1;
    }
    return levels;
}

[[nodiscard]] MarketEvent ParseSnapshotLine(std::string_view line) {
    MarketEvent event;
    if (const auto seq = JsonU64Field(line, "seq")) {
        event.seq = *seq;
    } else {
        throw std::runtime_error("jsonl: missing seq");
    }
    if (const auto ts = JsonU64Field(line, "ts_ns")) {
        event.ts = *ts;
    } else {
        throw std::runtime_error("jsonl: missing ts_ns");
    }
    if (const auto symbol = JsonStringField(line, "symbol")) {
        event.symbol = std::string(*symbol);
    }
    event.bids = JsonLevels(line, "bids");
    event.asks = JsonLevels(line, "asks");
    return event;
}

class JsonlFeed {
public:
    // If `only_symbol` is non-empty, lines for other symbols are skipped.
    explicit JsonlFeed(std::string path, std::string only_symbol = {})
        : path_(std::move(path))
        , only_symbol_(std::move(only_symbol))
        , file_(path_) {
        if (!file_) {
            throw std::runtime_error("cannot open dataset: " + path_);
        }
        pending_ = ReadOne();
        if (!pending_) {
            throw std::runtime_error(only_symbol_.empty()
                                         ? ("dataset is empty: " + path_)
                                         : ("dataset has no rows for symbol: " + only_symbol_));
        }
    }

    [[nodiscard]] SimTime StartTs() const { return pending_->ts; }

    [[nodiscard]] const std::string& FirstSymbol() const { return pending_->symbol; }

    [[nodiscard]] std::optional<MarketEvent> Next() {
        if (pending_) {
            MarketEvent event = std::move(*pending_);
            pending_.reset();
            return event;
        }
        return ReadOne();
    }

private:
    [[nodiscard]] std::optional<MarketEvent> ReadOne() {
        std::string line;
        while (std::getline(file_, line)) {
            if (line.empty()) {
                continue;
            }
            MarketEvent event = ParseSnapshotLine(line);
            if (!only_symbol_.empty() && !event.symbol.empty() && event.symbol != only_symbol_) {
                continue;
            }
            return event;
        }
        return std::nullopt;
    }

    std::string path_;
    std::string only_symbol_;
    std::ifstream file_;
    std::optional<MarketEvent> pending_;
};

struct Working {
    std::uint64_t client_id = 0;
    std::uint64_t order_id = 0;
    Side side = Side::Buy;
    long long price_ticks = 0;
    std::uint64_t leaves = 0;
};

class Venue {
public:
    Venue(Session& session, std::string class_code, std::string sec_code, double tick_size)
        : session_(session)
        , class_code_(std::move(class_code))
        , sec_code_(std::move(sec_code))
        , tick_size_(tick_size) {
        session_.on_new_order = [this](const cmf_driver::NewOrder& order) { OnNewOrder(order); };
        session_.on_cancel = [this](const cmf_driver::Cancel& cancel) { OnCancel(cancel); };
    }

    void Run(JsonlFeed& feed) {
        session_.Ready(feed.StartTs());

        while (auto event = feed.Next()) {
            session_.AdvanceTo(event->ts);

            if (session_.EngineFinished()) {
                break;
            }

            PublishAndMatch(*event);
        }

        session_.Finish();
    }

private:
    [[nodiscard]] long long ToTicks(double price) const {
        return static_cast<long long>(price / tick_size_ + (price < 0 ? -0.5 : 0.5));
    }

    [[nodiscard]] bool OnGrid(double price) const {
        return price > 0.0
            && std::abs(price - static_cast<double>(ToTicks(price)) * tick_size_)
                < tick_size_ / 1024.0;
    }

    void PublishAndMatch(const MarketEvent& event) {
        bids_ = event.bids;
        asks_ = event.asks;
        // Engine accepts at most 64 levels a side (TASK.md §5.2).
        if (bids_.size() > kMaxLevelsPerSide) {
            bids_.resize(kMaxLevelsPerSide);
        }
        if (asks_.size() > kMaxLevelsPerSide) {
            asks_.resize(kMaxLevelsPerSide);
        }

        PublishBook(event.seq, event.ts);
        MatchAgainstBook(event.ts);
    }

    // First update for the instrument is always S. Later updates are I diffs
    // when the change set fits in one message (≤64); otherwise another S.
    void PublishBook(std::uint64_t seq, SimTime ts) {
        if (!book_published_) {
            session_.PublishSnapshot(seq, ts, class_code_, sec_code_, bids_, asks_);
            book_published_ = true;
            last_pub_bids_ = bids_;
            last_pub_asks_ = asks_;
            return;
        }

        std::vector<LevelChange> changes;
        DiffSide(Side::Buy, last_pub_bids_, bids_, changes);
        DiffSide(Side::Sell, last_pub_asks_, asks_, changes);

        if (changes.empty()) {
            return;
        }

        // RequireDepth caps the whole I message at 64 changes.
        if (changes.size() > kMaxLevelsPerSide) {
            session_.PublishSnapshot(seq, ts, class_code_, sec_code_, bids_, asks_);
        } else {
            session_.PublishIncrement(seq, ts, class_code_, sec_code_, changes);
        }

        last_pub_bids_ = bids_;
        last_pub_asks_ = asks_;
    }

    // Absolute volumes; qty 0 removes a level. Prices compared in ticks.
    void DiffSide(Side side, const std::vector<Level>& before, const std::vector<Level>& after,
                  std::vector<LevelChange>& out) const {
        std::map<long long, Level> prev;
        std::map<long long, Level> next;
        for (const Level& level : before) {
            prev[ToTicks(level.price)] = level;
        }
        for (const Level& level : after) {
            next[ToTicks(level.price)] = level;
        }

        for (const auto& [ticks, level] : next) {
            const auto it = prev.find(ticks);
            if (it == prev.end() || it->second.qty != level.qty) {
                out.push_back(LevelChange{side, level.price, level.qty});
            }
        }
        for (const auto& [ticks, level] : prev) {
            if (next.find(ticks) == next.end()) {
                out.push_back(LevelChange{side, level.price, 0});
            }
        }
    }

    void OnNewOrder(const cmf_driver::NewOrder& order) {
        const SimTime effective = order.effective_ts;

        if (order.class_code != class_code_ || order.sec_code != sec_code_) {
            session_.Reject(order.client_id, effective, RejectReason::UnknownInstrument);
            return;
        }

        if (!OnGrid(order.price)) {
            session_.Reject(order.client_id, effective, RejectReason::PriceOffTickGrid);
            return;
        }

        if (order.qty == 0) {
            session_.Reject(order.client_id, effective, RejectReason::InvalidParameters);
            return;
        }

        // Protocol allows only 0/1/2; anything else is garbage from the engine side.
        if (order.tif != TimeInForce::Day && order.tif != TimeInForce::Ioc
            && order.tif != TimeInForce::Fok) {
            session_.Reject(order.client_id, effective, RejectReason::InvalidParameters);
            return;
        }

        const long long price_ticks = ToTicks(order.price);

        // FOK: all-or-nothing against the book right now. No Accept, no fills.
        if (order.tif == TimeInForce::Fok
            && !CanFullyFill(order.side, price_ticks, order.qty)) {
            session_.Reject(order.client_id, effective, RejectReason::InvalidParameters);
            return;
        }

        Working working;
        working.client_id = order.client_id;
        working.order_id = ++next_order_id_;
        working.side = order.side;
        working.price_ticks = price_ticks;
        working.leaves = order.qty;

        session_.Accept(working.client_id, effective, working.order_id, working.leaves);

        if (order.tif == TimeInForce::Day) {
            book_.push_back(working);
            MatchOne(book_.back(), effective);
        } else {
            // IOC / FOK: match now; never rest. IOC cancels any leftover.
            MatchOne(working, effective);
            if (working.leaves > 0) {
                session_.Cancelled(working.client_id, effective, working.order_id);
            }
        }
        Reap();
    }

    void OnCancel(const cmf_driver::Cancel& cancel) {
        const SimTime effective = cancel.effective_ts;

        for (Working& working : book_) {
            if (working.order_id == cancel.order_id && working.leaves > 0) {
                working.leaves = 0;
                session_.Cancelled(cancel.client_id, effective, working.order_id);
                Reap();
                return;
            }
        }

        session_.CancelRejected(cancel.client_id, effective, cancel.order_id,
                                RejectReason::NoSuchOrder);
    }

    void MatchAgainstBook(SimTime ts) {
        for (Working& working : book_) {
            MatchOne(working, ts);
        }
        Reap();
    }

    // True if walking opposite levels that cross can cover the whole qty (FOK check).
    // Does not mutate the book.
    [[nodiscard]] bool CanFullyFill(Side side, long long price_ticks, std::uint64_t qty) const {
        const std::vector<Level>& opposite = (side == Side::Buy) ? asks_ : bids_;
        std::uint64_t need = qty;

        for (const Level& level : opposite) {
            if (need == 0) {
                break;
            }
            if (level.qty == 0) {
                continue;
            }

            const long long level_ticks = ToTicks(level.price);
            const bool crosses = (side == Side::Buy) ? (price_ticks >= level_ticks)
                                                     : (price_ticks <= level_ticks);
            if (!crosses) {
                break;
            }

            if (level.qty >= need) {
                return true;
            }
            need -= level.qty;
        }

        return need == 0;
    }

    // Walk opposite levels while leaves remain and price still crosses.
    // Each fill is capped by that level's qty. DAY leftovers rest on book_;
    // IOC/FOK leftovers are handled by the caller (cancel / never accepted).
    void MatchOne(Working& working, SimTime ts) {
        // Buy lifts asks; sell hits bids. The other side is irrelevant here.
        std::vector<Level>& opposite = (working.side == Side::Buy) ? asks_ : bids_;

        while (working.leaves > 0 && !opposite.empty() && opposite.front().qty > 0) {
            Level& touch = opposite.front();
            const long long touch_ticks = ToTicks(touch.price);
            const bool crosses = (working.side == Side::Buy) ? (working.price_ticks >= touch_ticks)
                                                             : (working.price_ticks <= touch_ticks);
            if (!crosses) {
                break;
            }

            const std::uint64_t qty = std::min(working.leaves, touch.qty);
            working.leaves -= qty;
            touch.qty -= qty;

            session_.Fill(working.client_id, ts, working.order_id, ++next_trade_id_,
                          static_cast<double>(touch_ticks) * tick_size_, qty, working.leaves);

            if (touch.qty == 0) {
                opposite.erase(opposite.begin());
            }
        }
    }

    void Reap() {
        std::erase_if(book_, [](const Working& working) { return working.leaves == 0; });
    }

    static constexpr std::size_t kMaxLevelsPerSide = 64;

    Session& session_;
    std::string class_code_;
    std::string sec_code_;
    double tick_size_;

    std::vector<Level> bids_;
    std::vector<Level> asks_;
    // Last book we sent on the feed (before local match depletion).
    std::vector<Level> last_pub_bids_;
    std::vector<Level> last_pub_asks_;
    bool book_published_ = false;
    std::vector<Working> book_;

    std::uint64_t next_order_id_ = 9000;
    std::uint64_t next_trade_id_ = 70000;
};

} // namespace

void RunVenue(const AppConfig& cfg) {
    const cmf_driver::Options options = MakeListenOptions(cfg);

    // §2.4: bind both ports first (Session / Channel ctor), then load dataset,
    // then accept. Loading after HELLO keeps the engine waiting on a silent socket.
    Session session(options);
    std::fprintf(stderr, "venue: listening on %s, feed %u, orders %u\n", options.host.c_str(),
                 options.feed_port, options.order_port);

    // Resolve symbol before opening the filtered feed when config leaves it empty:
    // peek first row, then reopen with the filter.
    std::string sec = cfg.sec_code;
    if (sec.empty()) {
        sec = JsonlFeed(cfg.dataset).FirstSymbol();
    }
    if (sec.size() > 31) {
        throw std::runtime_error("sec_code longer than 31 chars: " + sec);
    }

    std::fprintf(stderr, "venue: JsonlFeed %s | %s/%s tick=%g\n", cfg.dataset.c_str(),
                 cfg.class_code.c_str(), sec.c_str(), cfg.tick_size);
    JsonlFeed feed(cfg.dataset, sec);

    const auto lookahead = session.Start();
    std::fprintf(stderr, "venue: engine declared lookahead %llu ns\n",
                 static_cast<unsigned long long>(lookahead));

    Venue(session, cfg.class_code, std::move(sec), cfg.tick_size).Run(feed);
    std::fprintf(stderr, "venue: finished, said E on both channels\n");
}
