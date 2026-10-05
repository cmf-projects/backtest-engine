#pragma once

// ============================================================================
// CMF driver -- the transport and the clock discipline of the backtest
// protocol, so that a venue only has to be a venue.
//
// Read README.md next to this file first. It is the protocol; this is one
// implementation of the half that is the same for everybody.
//
// Depends on standalone asio and the standard library. Nothing else, on
// purpose: this file is meant to be dropped into your project and compiled
// there.
//
//   #define ASIO_STANDALONE   (or link the `asio` interface target)
//   c++20
//
// WHAT IT DOES FOR YOU
//
//   * parses the command line the engine starts you with;
//   * binds and accepts both channels, in the right order;
//   * frames, encodes and decodes every message, including the price rule,
//     which is easy to get subtly wrong;
//   * runs the conservative-synchronisation handshake: it will not let you
//     advance past what the engine has allowed, and it announces where you
//     stand before it blocks, which is what keeps the two sides from waiting
//     on each other forever.
//
// WHAT IT DOES NOT DO
//
//   Anything that is a venue's job: reading your datasets, keeping a book,
//   matching, deciding whether a price is on your tick grid. Those are the
//   exercise.
// ============================================================================

#include <asio.hpp>

#include <algorithm>
#include <charconv>
#include <deque>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cmf_driver {

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------

/// Logical time, nanoseconds. Unsigned, and both ends of the range are
/// reserved: 0 means "not stamped" and the maximum means "this channel has
/// ended". Start your dataset at 1 or, more usefully, at a real epoch stamp.
using SimTime = std::uint64_t;

inline constexpr SimTime kTimeUnset = 0;
inline constexpr SimTime kTimeInfinity = UINT64_MAX;

enum class Side : char { Buy = 'B', Sell = 'S' };

/// What the engine wants done with an order it could not fill outright.
/// Travels as its numeric value.
enum class TimeInForce : std::uint8_t { Day = 0, Ioc = 1, Fok = 2 };

/// Why you refused something. The engine treats every one of these the same
/// way, but a human reading the log does not, so pick the one that is true.
enum class RejectReason : std::uint32_t {
    NoSuchOrder = 1,
    OrderIsTerminal = 2,

    /// The price is not a multiple of your tick. REFUSE, never round: the
    /// engine rounds to a tick of its own, and if the two grids differ then
    /// every quote lands on a price your book does not have and nothing
    /// matches -- silently, for the whole run. Refusing turns that into a loud
    /// failure on the very first order.
    PriceOffTickGrid = 3,

    InvalidParameters = 4,
    UnknownInstrument = 5,
    InternalError = 9,
};

/// One price level. Volume is a count, never a measurement: exactly zero means
/// "this level is gone".
struct Level {
    double price = 0.0;
    std::uint64_t qty = 0;
};

/// One level of an incremental update.
struct LevelChange {
    Side side = Side::Buy;
    double price = 0.0;
    std::uint64_t qty = 0;
};

/// An order the engine wants placed.
struct NewOrder {
    /// Opaque. Echo it back byte for byte in every reply about this order --
    /// it is the only thing that attributes a reply to a request. Do not
    /// parse it, do not use it as the order's key in your own book: an order
    /// and its cancel carry different values here.
    std::uint64_t client_id = 0;

    /// The logical instant the engine was at when it decided to send this.
    SimTime seen_ts = kTimeUnset;

    /// When it takes effect in your simulation: seen_ts + lookahead. This is
    /// the number to schedule on. Filling it any earlier is inventing latency
    /// the engine was promised it would not get.
    SimTime effective_ts = kTimeUnset;

    std::string class_code;
    std::string sec_code;
    Side side = Side::Buy;
    double price = 0.0;
    std::uint64_t qty = 0;
    TimeInForce tif = TimeInForce::Day;
};

/// A cancel the engine wants applied.
struct Cancel {
    /// The cancel's OWN id -- not the order's. Echo it back.
    std::uint64_t client_id = 0;

    SimTime seen_ts = kTimeUnset;
    SimTime effective_ts = kTimeUnset;

    std::string class_code;
    std::string sec_code;

    /// The order being cancelled, by the id YOU issued for it.
    ///
    /// May be 0, and that is not a malformed line: on the way out the engine
    /// pulls everything it still has open, including an order you have not
    /// answered yet and therefore have no id for. Answer with
    /// CancelRejected(..., RejectReason::NoSuchOrder). Do not fall over.
    std::uint64_t order_id = 0;
};

/// Raised on anything that means the two sides no longer agree about the
/// protocol. There is no sensible way to carry on from one of these.
class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& what) : std::runtime_error(what) {}
};

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

/// What the engine starts you with. See README.md section "How you are
/// launched" for the exact spelling and for what is guaranteed about it.
struct Options {
    /// The address to LISTEN on. You are the server; the engine dials in.
    std::string host = "127.0.0.1";

    unsigned short feed_port = 8080;
    unsigned short order_port = 8081;

    /// Whatever the operator named on the command line, in order. Usually
    /// dataset paths; the engine passes them through untouched and has no
    /// opinion about them.
    std::vector<std::string> datasets;
};

namespace detail {

inline bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

template <typename T>
bool ParseWhole(std::string_view text, T& out) {
    if (text.empty()) {
        return false;
    }

    const auto [stop, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && stop == text.data() + text.size();
}

inline std::string_view Trim(std::string_view line) {
    while (!line.empty()
           && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }
    return line;
}

/// Splits off the next '|'-separated field, consuming it.
inline std::string_view NextField(std::string_view& rest) {
    const auto pos = rest.find('|');

    if (pos == std::string_view::npos) {
        const std::string_view all = rest;
        rest = {};
        return all;
    }

    const std::string_view head = rest.substr(0, pos);
    rest.remove_prefix(pos + 1);
    return head;
}

template <typename T>
T Field(std::string_view& rest, const char* what) {
    T value{};

    if (!ParseWhole(NextField(rest), value)) {
        throw ProtocolError(std::string("cannot read field: ") + what);
    }

    return value;
}

/// A price on the wire: plain decimal, never a shortest round-trip double.
///
/// A price computed as round(x / tick) * tick often lands one unit in the last
/// place away from the nearest double to the decimal you meant. Printing the
/// shortest representation then spells that out in full -- `239.64` becomes
/// `239.64000000000001`, which is not on anyone's tick grid. So: fixed
/// notation with room to spare, then trim the zeros the error hides under.
inline std::string FormatPrice(double value) {
    char buffer[64];

    const auto [stop, error] =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 9);

    if (error != std::errc{}) {
        throw ProtocolError("price does not fit its buffer");
    }

    std::string text(buffer, static_cast<std::size_t>(stop - buffer));

    const std::size_t last = text.find_last_not_of('0');
    text.erase((text[last] == '.') ? last : last + 1);

    return text;
}

inline double ParsePrice(std::string_view text) {
    // `nan` and `inf` are valid spellings of a double and are not prices. A NaN
    // does not trip a comparison, it makes every comparison false, so one that
    // gets in stops being visible until something built on it is wrong.
    for (const char c : text) {
        const bool decimal = (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+'
                          || c == 'e' || c == 'E';
        if (!decimal) {
            throw ProtocolError("price is not a decimal number");
        }
    }

    double value = 0.0;
    if (!ParseWhole(text, value)) {
        throw ProtocolError("cannot read price");
    }

    return value;
}

} // namespace detail

/// @brief Reads the engine's command line. Unknown arguments become datasets.
///
/// Lenient on purpose: the engine may grow a flag before your build has caught
/// up, and a venue that refuses to start over one it does not recognise is
/// worse than one that ignores it. The connection parameters are the only ones
/// with meaning here, and they always arrive.
inline Options ParseOptions(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];

        if (detail::StartsWith(argument, "--host=")) {
            options.host = std::string(argument.substr(7));
        } else if (detail::StartsWith(argument, "--feed-port=")) {
            if (!detail::ParseWhole(argument.substr(12), options.feed_port)) {
                throw ProtocolError("bad --feed-port");
            }
        } else if (detail::StartsWith(argument, "--order-port=")) {
            if (!detail::ParseWhole(argument.substr(13), options.order_port)) {
                throw ProtocolError("bad --order-port");
            }
        } else if (!detail::StartsWith(argument, "--")) {
            options.datasets.emplace_back(argument);
        }
    }

    return options;
}

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------

/// @brief One line-framed TCP connection, synchronous.
///
/// Synchronous because the subject is the protocol, not asio: completion
/// handlers would add a second unfamiliar thing at the same time as the first.
/// A venue is not latency-critical -- it is the thing being measured against,
/// and its clock is logical.
class Channel {
public:
    Channel(asio::io_context& io, const std::string& host, unsigned short port)
        // Bound in the constructor, and that matters: the engine starts you and
        // then dials in, so a venue that does its dataset loading before it
        // binds can miss the connection entirely.
        //
        // This asio constructor also sets SO_REUSEADDR for us, BEFORE it binds,
        // which is the only moment the option means anything -- it is what lets
        // you re-run straight after a Ctrl-C instead of waiting out TIME_WAIT.
        // Setting it afterwards, on an acceptor that has already bound, is a
        // no-op that looks like the thing doing the work.
        : acceptor_(io, asio::ip::tcp::endpoint(asio::ip::make_address(host), port))
        , socket_(io) {}

    /// Blocks until the engine connects. Call once.
    void Accept() {
        acceptor_.accept(socket_);
        socket_.set_option(asio::ip::tcp::no_delay(true));
    }

    /// @brief Blocks until a whole line arrives.
    /// @return the line without its newline, or nothing once the peer is gone.
    std::optional<std::string> ReadLine() {
        asio::error_code error;
        const std::size_t n = asio::read_until(socket_, asio::dynamic_buffer(buffer_), '\n', error);

        if (error) {
            return std::nullopt;
        }

        std::string line = buffer_.substr(0, n - 1);
        buffer_.erase(0, n);
        return line;
    }

    /// @brief A line, but only if one is already there.
    ///
    /// What lets a venue drain orders between market events instead of
    /// stalling on a read that may never return.
    std::optional<std::string> TryReadLine() {
        if (buffer_.find('\n') == std::string::npos) {
            asio::error_code error;
            const std::size_t available = socket_.available(error);

            if (error || available == 0) {
                return std::nullopt;
            }

            const std::size_t had = buffer_.size();
            buffer_.resize(had + available);
            const std::size_t got = socket_.read_some(
                asio::buffer(buffer_.data() + had, available), error);
            buffer_.resize(had + got);

            if (error || buffer_.find('\n') == std::string::npos) {
                return std::nullopt;
            }
        }

        const std::size_t newline = buffer_.find('\n');
        std::string line = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        return line;
    }

    void Write(const std::string& line) {
        asio::write(socket_, asio::buffer(line));
    }

private:
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    std::string buffer_;
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/// @brief Both channels, the clock discipline, and the message codec.
///
/// The rule the whole thing exists to enforce, in one line: YOU MAY NOT
/// SIMULATE PAST `EngineTime() + Lookahead()`. AdvanceTo() is where that is
/// enforced, and it is the only place a venue has to think about it.
class Session {
public:
    /// Called when the engine sends an order. Reply from inside it, or store
    /// the order and reply when it is due -- either is fine, as long as no
    /// reply is stamped earlier than `effective_ts`.
    std::function<void(const NewOrder&)> on_new_order;

    /// Called when the engine cancels. Note that `order_id` may be 0.
    std::function<void(const Cancel&)> on_cancel;

    explicit Session(const Options& options)
        : feed_(io_, options.host, options.feed_port)
        , orders_(io_, options.host, options.order_port) {}

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// @brief Ends both channels if you have not already.
    ///
    /// Forgetting `Finish()` is the one omission with no symptom of its own:
    /// the engine cannot tell a finished dataset from a venue that died, so it
    /// waits, and the run ends on a timeout minutes later with nothing pointing
    /// at the cause. Doing it here means the ordinary path -- return from
    /// `main`, let the session go out of scope -- is always correct.
    ///
    /// It runs on the way out of an error too, and that is deliberate: `E` says
    /// "nothing more on this channel", which is true however you are leaving.
    /// Whatever went wrong is on your stderr and in your exit code, where it
    /// belongs; making the engine sit out a timeout as well tells nobody
    /// anything new.
    ~Session() {
        try {
            Finish();
        } catch (...) {
            // The peer may already be gone, which is not worth a terminate on
            // the way out.
        }
    }

    /// @brief Accepts both connections and reads the handshake.
    ///
    /// The order channel is accepted first because that is the one the engine
    /// dials first, and the handshake arrives on it before any market data
    /// would be wanted.
    ///
    /// @return the declared lookahead, in nanoseconds. Always positive.
    SimTime Start() {
        orders_.Accept();
        feed_.Accept();

        // HELLO first, always. It carries the number both sides measure the
        // run at, and taking it off the wire rather than agreeing on a
        // constant is what stops the two halves from quietly disagreeing.
        while (lookahead_ == 0) {
            const std::optional<std::string> line = orders_.ReadLine();

            if (!line) {
                throw ProtocolError("engine closed the order channel before the handshake");
            }

            HandleEngineLine(detail::Trim(*line));
        }

        return lookahead_;
    }

    [[nodiscard]] SimTime Lookahead() const { return lookahead_; }

    /// @brief The last time the engine announced.
    ///
    /// You may run STRICTLY BELOW `this + lookahead`, never up to it: a request
    /// stamped with the announced time itself comes due at exactly that bound
    /// and may still be on its way. `AdvanceTo` enforces it; this accessor is
    /// here for a venue that wants to look.
    [[nodiscard]] SimTime EngineTime() const { return engine_time_; }

    /// @brief Whether the engine has said it is finished.
    [[nodiscard]] bool EngineFinished() const { return engine_finished_; }

    /// @brief "The account is described, you may trade" -- and the first
    ///        horizon on both channels.
    ///
    /// Send this ONCE, before anything else, with the timestamp your dataset
    /// starts at. Until it arrives the engine holds every order back, so a
    /// venue that forgets it streams a perfect book into an engine that will
    /// never trade.
    void Ready(SimTime start_ts) {
        RequireStamp(start_ts, "Ready");

        // The origin of the run is YOURS to choose. Until the engine has
        // processed something it has no clock of its own, so it cannot grant
        // permission to reach a time it has never heard of -- the first stamp
        // it ever sees is this one, and everything is measured from it.
        start_ts_ = start_ts;
        venue_time_ = start_ts;

        orders_.Write("K|" + std::to_string(start_ts) + "\n");
        order_sent_ = start_ts;

        // The order channel is finished with `start_ts` -- `K` was the last
        // thing on it at that instant and a reply cannot be due before
        // start_ts + lookahead. So the promise is honest, and the engine needs
        // it: with one channel silent it cannot act on the other at all.
        //
        // The FEED gets no promise here, and that is the point. A snapshot at
        // exactly `start_ts` is about to go out on it, and `H|X` means "nothing
        // more at or below X" -- announcing it here would be false before the
        // next line of your own code runs. The feed needs no promise anyway:
        // that snapshot is its own clock.
        AnnounceOrders(start_ts);
    }

    /// @brief Moves venue time to `ts`, delivering everything due on the way.
    ///
    /// The one call that matters, and it does three things in this order:
    ///
    ///   1. reads whatever the engine has sent, into a queue -- NOT into your
    ///      handlers;
    ///   2. blocks while `ts` is at or past what the engine has allowed,
    ///      announcing where it stands, until it is allowed;
    ///   3. calls your handlers for every request whose effective time has now
    ///      arrived, with the venue clock standing at that instant.
    ///
    /// The split between 1 and 3 is the important part and it is not an
    /// optimisation. A request is handed over when its EFFECTIVE TIME comes
    /// round, not when its bytes turn up -- those are different instants, a
    /// whole lookahead apart. Delivering on arrival means a venue answers an
    /// order at `seen + lookahead` and then, a moment later, fills something
    /// older at the current market time, which is EARLIER; the order channel's
    /// stamps go backwards and the engine stops the run. Every venue would hit
    /// this, and it is not the venue's problem to solve twice.
    ///
    /// And 2 comes BEFORE 3 for the reason spelled out at the loop: the queue
    /// is only complete for `ts` once `ts` is strictly inside the window.
    void AdvanceTo(SimTime ts) {
        RequireStamp(ts, "AdvanceTo");

        if (ts < venue_time_) {
            throw ProtocolError("venue time would go backwards: " + std::to_string(venue_time_)
                                + " -> " + std::to_string(ts));
        }

        DrainOrders();

        // `>=`, not `>`, and the nanosecond is the whole point.
        //
        // `Allowed()` is `announced + lookahead`, and a request stamped with
        // the engine's own announced time may still be on its way: the engine
        // announces a time and then processes a message at exactly it. Such a
        // request comes due at `announced + lookahead` -- the limit ITSELF. So
        // the limit is a time this venue may run UP TO and not INTO.
        //
        // Letting it in was a race with no symptom. The request turns up either
        // side of `book.apply()` for that same instant depending on when its
        // bytes land, so it is matched against two different books; one run in
        // three filled an order the others cancelled, and every number after
        // that point differed. A venue cannot defend itself against this -- by
        // the time the handler runs the damage is done -- which is why the
        // limit is enforced here rather than documented as a caveat.
        //
        // Cost: the window advances by `lookahead - 1` per round trip instead
        // of `lookahead`. Nothing at a sane lookahead, and it is why the engine
        // refuses a lookahead of one as flatly as it refuses zero.
        while (ts >= Allowed() && !engine_finished_) {
            if (Allowed() > 0) {
                // Everything that can no longer change hands goes out BEFORE
                // the promise covering it -- otherwise the promise is a lie the
                // moment a queued request is answered under it. Strictly below
                // the limit, for the reason above: at the limit itself another
                // request may still arrive, and answering one of the two first
                // because the socket delivered it first is the same race.
                DeliverDue(Allowed() - 1);
                AnnounceOrders(Allowed() - 1);
            }

            // The feed promise is about OUR data, and we know exactly where it
            // stands: everything below `ts` has been published and the next
            // thing is at `ts`. That is both true and the most this venue can
            // say -- announcing the limit instead would be a lie precisely when
            // we are held AT it, with the event at that instant still to come.
            AnnounceFeed(ts - 1);

            WaitForEngine();
            DrainOrders();
        }

        // Only now, with the limit strictly above `ts`, is the queue complete
        // for this instant.
        DeliverDue(ts);
        venue_time_ = ts;
    }

    /// @brief Reads whatever the engine has already sent, without blocking.
    ///
    /// Parses and queues. Your handlers are called by DeliverDue, when the
    /// clock reaches each request, and never from here.
    void DrainOrders() {
        while (const std::optional<std::string> line = orders_.TryReadLine()) {
            HandleEngineLine(detail::Trim(*line));
        }
    }

    /// @brief Hands over every queued request that has come due at or before
    ///        `upto`, with the venue clock set to each one's effective time.
    ///
    /// Called for you by AdvanceTo. Public only so a venue that drives its own
    /// loop differently can still get the ordering right.
    void DeliverDue(SimTime upto) {
        while (!pending_.empty() && pending_.front().effective_ts <= upto) {
            const Pending request = pending_.front();
            pending_.pop_front();

            // The clock stands at the request's own instant while the handler
            // runs, so a reply stamped `effective_ts` is stamped "now" and
            // every reply comes out in time order.
            venue_time_ = std::max(venue_time_, request.effective_ts);

            // A missing handler is a broken venue, not a venue that declines.
            // Every command needs a reply, so dropping one silently produces a
            // run where the engine's orders simply evaporate: the report says
            // they went unanswered and nothing anywhere says why.
            if (request.is_cancel) {
                if (!on_cancel) {
                    throw ProtocolError("the engine sent a cancel and on_cancel is not set");
                }
                on_cancel(request.cancel);
            } else {
                if (!on_new_order) {
                    throw ProtocolError("the engine sent an order and on_new_order is not set");
                }
                on_new_order(request.order);
            }
        }
    }

    // -- market data ------------------------------------------------------

    /// @brief A full book. The first message about any instrument must be one.
    void PublishSnapshot(std::uint64_t seq, SimTime ts, std::string_view class_code,
                         std::string_view sec_code, const std::vector<Level>& bids,
                         const std::vector<Level>& asks) {
        RequireStamp(ts, "PublishSnapshot");
        RequireDepth(bids.size(), "bids");
        RequireDepth(asks.size(), "asks");

        std::string line = "S|" + std::to_string(seq) + "|" + std::to_string(ts) + "|";
        line.append(class_code).append("|").append(sec_code).append("|");
        line += std::to_string(bids.size()) + "|" + std::to_string(asks.size());

        for (const Level& level : bids) {
            line += "|" + detail::FormatPrice(level.price) + ":" + std::to_string(level.qty);
        }

        for (const Level& level : asks) {
            line += "|" + detail::FormatPrice(level.price) + ":" + std::to_string(level.qty);
        }

        WriteFeed(line, ts);
    }

    /// @brief A change to a book that already exists.
    ///
    /// One of these is one atomic change, however many levels it names.
    /// Volumes are absolute, and a volume of exactly 0 removes the level.
    void PublishIncrement(std::uint64_t seq, SimTime ts, std::string_view class_code,
                          std::string_view sec_code, const std::vector<LevelChange>& changes) {
        RequireStamp(ts, "PublishIncrement");
        RequireDepth(changes.size(), "changes");

        std::string line = "I|" + std::to_string(seq) + "|" + std::to_string(ts) + "|";
        line.append(class_code).append("|").append(sec_code).append("|");
        line += std::to_string(changes.size());

        for (const LevelChange& change : changes) {
            line += "|";
            line += static_cast<char>(change.side);
            line += ":" + detail::FormatPrice(change.price) + ":" + std::to_string(change.qty);
        }

        WriteFeed(line, ts);
    }

    // -- replies ----------------------------------------------------------

    /// @brief The order is on your book. Must precede every fill or cancel of it.
    ///
    /// `order_id` is yours to issue: non-zero, and the same for the order's
    /// whole life. `leaves` is what is left working after any immediate fill.
    void Accept(std::uint64_t client_id, SimTime ts, std::uint64_t order_id, std::uint64_t leaves) {
        RequireOrderId(order_id, "Accept");
        WriteReply("A|" + std::to_string(client_id) + "|" + Stamp(ts) + "|"
                   + std::to_string(order_id) + "|" + std::to_string(leaves));
    }

    /// @brief The order was refused and never existed. No id is issued.
    void Reject(std::uint64_t client_id, SimTime ts, RejectReason reason) {
        WriteReply("R|" + std::to_string(client_id) + "|" + Stamp(ts) + "|"
                   + std::to_string(static_cast<std::uint32_t>(reason)));
    }

    /// @brief One match event.
    ///
    /// `trade_id` must be non-zero and unique for the run -- the engine
    /// deduplicates on it, and a repeat would be counted twice. `leaves` must
    /// be exact: it is what tells the engine the order is finished.
    void Fill(std::uint64_t client_id, SimTime ts, std::uint64_t order_id, std::uint64_t trade_id,
              double price, std::uint64_t qty, std::uint64_t leaves) {
        RequireOrderId(order_id, "Fill");

        if (trade_id == 0) {
            throw ProtocolError("Fill: trade_id must not be zero");
        }

        WriteReply("F|" + std::to_string(client_id) + "|" + Stamp(ts) + "|"
                   + std::to_string(order_id) + "|" + std::to_string(trade_id) + "|"
                   + detail::FormatPrice(price) + "|" + std::to_string(qty) + "|"
                   + std::to_string(leaves));
    }

    /// @brief The order is off the book. Nothing further about it may follow.
    void Cancelled(std::uint64_t client_id, SimTime ts, std::uint64_t order_id) {
        RequireOrderId(order_id, "Cancelled");
        WriteReply("X|" + std::to_string(client_id) + "|" + Stamp(ts) + "|"
                   + std::to_string(order_id));
    }

    /// @brief The CANCEL failed. Says nothing about the order, which is still live.
    ///
    /// What became of it arrives later, through a fill or a cancel. There is
    /// deliberately no "is it still alive" field: from a refusal alone that is
    /// unknowable, and a field that is right half the time is worse than none.
    void CancelRejected(std::uint64_t client_id, SimTime ts, std::uint64_t order_id,
                        RejectReason reason) {
        WriteReply("Y|" + std::to_string(client_id) + "|" + Stamp(ts) + "|"
                   + std::to_string(order_id) + "|"
                   + std::to_string(static_cast<std::uint32_t>(reason)));
    }

    // -- end --------------------------------------------------------------

    /// @brief Promises both channels that nothing with ts <= `upto` follows.
    ///
    /// Note "<=", not "<". The promise has to rule out `upto` itself, because
    /// what the engine does with it is take the OTHER channel up to and
    /// including that instant -- and if a message at exactly `upto` could still
    /// turn up here, which of the two it processed first would be decided by
    /// the socket rather than by the model.
    ///
    /// Only call it when it is true. Inside AdvanceTo it is: everything due at
    /// or below the limit has been delivered and answered, and the next market
    /// event is past it.
    void AnnounceHorizons(SimTime upto) {
        AnnounceFeed(upto);
        AnnounceOrders(upto);
    }

    void AnnounceFeed(SimTime upto) {
        if (upto <= feed_announced_) {
            return;
        }

        feed_.Write("H|" + std::to_string(upto) + "\n");
        feed_announced_ = upto;
        feed_sent_ = std::max(feed_sent_, upto);
    }

    void AnnounceOrders(SimTime upto) {
        if (upto <= order_announced_) {
            return;
        }

        orders_.Write("H|" + std::to_string(upto) + "\n");
        order_announced_ = upto;
        order_sent_ = std::max(order_sent_, upto);
    }

    /// @brief The furthest logical time you are currently allowed to reach.
    ///
    /// Before the engine has announced anything it has no clock, so the origin
    /// of the run is whatever Ready() named -- there is nothing else it could
    /// be measured from.
    [[nodiscard]] SimTime Allowed() const {
        return (engine_time_ == kTimeUnset) ? start_ts_ : engine_time_ + lookahead_;
    }

    /// @brief The dataset is finished. The ONLY clean end of a run.
    ///
    /// Going quiet is not an end: the engine cannot tell a finished dataset
    /// from a venue that died, so it waits, and eventually reports a
    /// successful run as a lost connection.
    /// Idempotent: the destructor calls it too, and `E` twice on one channel
    /// would break the rule that nothing follows it.
    void Finish() {
        if (finished_) {
            return;
        }

        finished_ = true;
        feed_.Write("E\n");
        orders_.Write("E\n");
    }

    // DO NOT drain and deliver the queue here before the `E`s. It looks like an
    // improvement -- a request the engine sent as the data ran out comes due
    // past your last event, so nobody ever answers it and the engine's report
    // says one command went unanswered. Answering it seems strictly better.
    //
    // It is not, and the reason is the whole point of this protocol. WHICH
    // requests have arrived by the time you get here is decided by the socket,
    // not by the model: the engine is still working through its own backlog and
    // emitting orders while you are finishing. Delivering "whatever turned up"
    // makes the set of answered requests -- and with it the engine's final
    // clock, its reply count and its report -- depend on process scheduling.
    // Measured: 6 runs in 20 of one dataset answered one cancel the other 14
    // did not, and every number after that differed.
    //
    // One unanswered command at the tail of a run is the honest outcome. The
    // simulated world ended; there is no instant left to answer in.

private:
    void WriteFeed(const std::string& line, SimTime ts) {
        // Same rule as a reply, on the other channel: a market event at or
        // below a time already promised clear is the promise being broken.
        if (ts <= feed_announced_) {
            throw ProtocolError("market event stamped " + std::to_string(ts)
                                + ", at or before the feed's own promise ("
                                + std::to_string(feed_announced_) + ")");
        }

        if (ts < feed_sent_) {
            throw ProtocolError("market event stamped " + std::to_string(ts)
                                + ", earlier than the feed has already reached ("
                                + std::to_string(feed_sent_) + ")");
        }

        feed_.Write(line + "\n");
        feed_sent_ = ts;
    }

    void WriteReply(const std::string& body) {
        orders_.Write(body + "\n");
    }

    std::string Stamp(SimTime ts) {
        RequireStamp(ts, "reply");

        if (ts < venue_time_) {
            throw ProtocolError("reply stamped before the current venue time: "
                                + std::to_string(ts) + " < " + std::to_string(venue_time_));
        }

        // The order channel carries non-decreasing stamps, so a reply earlier
        // than one already sent -- or earlier than a horizon already promised
        // -- is a broken promise. Caught here rather than by the engine: the
        // same mistake reported from inside your own process names the reply
        // that did it, and the engine can only say that some stamp went
        // backwards several thousand messages ago.
        //
        // Reaching this after using AdvanceTo means a reply was stamped with
        // something other than the instant it was handed over at.
        if (ts < order_sent_) {
            throw ProtocolError("reply stamped " + std::to_string(ts)
                                + ", earlier than the order channel has already reached ("
                                + std::to_string(order_sent_)
                                + ") -- replies must go out in time order");
        }

        // A reply at an instant this channel has already promised it was
        // finished with. Reached by deferring a reply past the announcement
        // that covered it -- if you queue replies rather than sending them from
        // the handler, they have to go out before the clock is allowed past.
        if (ts <= order_announced_) {
            throw ProtocolError("reply stamped " + std::to_string(ts)
                                + ", at or before the order channel's own promise ("
                                + std::to_string(order_announced_) + ")");
        }

        order_sent_ = ts;
        return std::to_string(ts);
    }

    static void RequireStamp(SimTime ts, const char* where) {
        if (ts == kTimeUnset || ts == kTimeInfinity) {
            throw ProtocolError(std::string(where) + ": 0 and UINT64_MAX are reserved timestamps");
        }
    }

    static void RequireOrderId(std::uint64_t order_id, const char* where) {
        if (order_id == 0) {
            throw ProtocolError(std::string(where) + ": order_id must not be zero");
        }
    }

    static void RequireDepth(std::size_t levels, const char* what) {
        // The engine keeps 64 levels a side. A book deeper than that has to be
        // published as its top 64, not truncated silently somewhere further on.
        if (levels > 64) {
            throw ProtocolError(std::string("more than 64 ") + what + " in one message");
        }
    }

    /// Blocks until the engine says something. Only ever called when there is
    /// nothing else we are allowed to do.
    void WaitForEngine() {
        const std::optional<std::string> line = orders_.ReadLine();

        if (!line) {
            // The channel closed. Whether that is the end of the run or the
            // engine dying is decided by one thing: whether `BYE` came first.
            //
            // Treating every close as a clean end is the trap. The engine stops
            // where a protocol invariant breaks -- and writes no report when it
            // does -- so a venue that read a closed socket as "finished" would
            // exit zero saying the dataset was replayed, over its own bug, with
            // the one document that named the bug never written. That is the
            // wrong end of a debugging session to start from.
            if (!engine_finished_) {
                throw ProtocolError(
                    "the engine closed the order channel without saying BYE -- it did not "
                    "finish, it stopped. Its stderr says why: a broken protocol invariant "
                    "halts it there and no report is written");
            }

            return;
        }

        HandleEngineLine(detail::Trim(*line));
    }

    void HandleEngineLine(std::string_view line) {
        if (line.empty()) {
            return;
        }

        if (detail::StartsWith(line, "TIME|")) {
            line.remove_prefix(5);
            const SimTime announced = detail::Field<SimTime>(line, "TIME");

            // Monotonic by construction on the engine's side; taking the max
            // means a duplicate or a reordering cannot walk our limit back.
            engine_time_ = std::max(engine_time_, announced);
            return;
        }

        if (detail::StartsWith(line, "HELLO|")) {
            line.remove_prefix(6);
            const SimTime declared = detail::Field<SimTime>(line, "HELLO");

            if (declared == 0) {
                throw ProtocolError("engine declared a zero lookahead");
            }

            lookahead_ = declared;
            return;
        }

        if (line == "BYE") {
            engine_finished_ = true;
            return;
        }

        switch (line[0]) {
        case 'N': ReadNewOrder(line); return;
        case 'C': ReadCancel(line); return;
        default: break;
        }

        throw ProtocolError("unknown line from the engine: " + std::string(line));
    }

    void ReadNewOrder(std::string_view line) {
        line.remove_prefix(2);

        NewOrder order;
        order.client_id = detail::Field<std::uint64_t>(line, "N.client_id");
        order.seen_ts = detail::Field<SimTime>(line, "N.ts_seen");
        order.class_code = std::string(detail::NextField(line));
        order.sec_code = std::string(detail::NextField(line));

        const std::string_view side = detail::NextField(line);
        if (side.size() != 1 || (side[0] != 'B' && side[0] != 'S')) {
            throw ProtocolError("N: side is neither B nor S");
        }
        order.side = static_cast<Side>(side[0]);

        order.price = detail::ParsePrice(detail::NextField(line));
        order.qty = detail::Field<std::uint64_t>(line, "N.qty");
        order.tif = static_cast<TimeInForce>(detail::Field<std::uint8_t>(line, "N.tif"));
        order.effective_ts = order.seen_ts + lookahead_;

        RequireNotLate(order.effective_ts, "order");
        Enqueue(Pending{.effective_ts = order.effective_ts, .is_cancel = false,
                        .order = order, .cancel = {}});
    }

    void ReadCancel(std::string_view line) {
        line.remove_prefix(2);

        Cancel cancel;
        cancel.client_id = detail::Field<std::uint64_t>(line, "C.client_id");
        cancel.seen_ts = detail::Field<SimTime>(line, "C.ts_seen");
        cancel.class_code = std::string(detail::NextField(line));
        cancel.sec_code = std::string(detail::NextField(line));
        cancel.order_id = detail::Field<std::uint64_t>(line, "C.order_id");
        cancel.effective_ts = cancel.seen_ts + lookahead_;

        RequireNotLate(cancel.effective_ts, "cancel");
        Enqueue(Pending{.effective_ts = cancel.effective_ts, .is_cancel = true,
                        .order = {}, .cancel = cancel});
    }

    /// A request whose effective time has already passed cannot be simulated
    /// honestly -- the book has moved on. It should be impossible: the engine
    /// sends an order before it announces the time that would let us past it.
    /// Loud rather than silent, because the alternative is an invented fill.
    void RequireNotLate(SimTime effective_ts, const char* what) const {
        if (effective_ts < venue_time_) {
            throw ProtocolError(std::string(what) + " arrived after its effective time: "
                                + std::to_string(effective_ts) + " < "
                                + std::to_string(venue_time_));
        }
    }

    /// One request waiting for its instant. Kept whole rather than as a
    /// variant so a venue reading a crash dump sees the fields it knows.
    struct Pending {
        SimTime effective_ts = kTimeUnset;
        bool is_cancel = false;
        NewOrder order;
        Cancel cancel;
    };

    /// A plain FIFO is enough, and that is a property of the protocol rather
    /// than luck: `effective_ts` is the engine's own clock plus a constant, the
    /// engine's clock never goes backwards, and one channel keeps its order.
    /// Checked anyway -- an engine that broke it would be delivered out of
    /// order and nothing downstream could tell.
    void Enqueue(Pending request) {
        if (!pending_.empty() && request.effective_ts < pending_.back().effective_ts) {
            throw ProtocolError("engine sent requests out of order: "
                                + std::to_string(request.effective_ts) + " after "
                                + std::to_string(pending_.back().effective_ts));
        }

        pending_.push_back(std::move(request));
    }

    asio::io_context io_;
    Channel feed_;
    Channel orders_;

    std::deque<Pending> pending_;

    SimTime lookahead_ = 0;
    SimTime start_ts_ = kTimeUnset;
    SimTime engine_time_ = kTimeUnset;
    SimTime venue_time_ = kTimeUnset;
    /// The highest stamp actually put on each channel, and the highest each
    /// has PROMISED to be finished with. Two different things: sending a
    /// message at T says nothing about whether another may follow at T, and
    /// only the promise rules that out.
    SimTime feed_sent_ = kTimeUnset;
    SimTime order_sent_ = kTimeUnset;
    SimTime feed_announced_ = kTimeUnset;
    SimTime order_announced_ = kTimeUnset;
    bool engine_finished_ = false;
    bool finished_ = false;
};

} // namespace cmf_driver
