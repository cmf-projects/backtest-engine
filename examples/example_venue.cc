// ============================================================================
// A complete venue in one file, built on the CMF driver.
//
// It is deliberately the smallest thing that is still HONEST about the
// protocol: one instrument, a book that walks on its own instead of coming out
// of a dataset, and a matcher that fills anything crossing the touch. Every
// rule a real venue has to keep is kept here, so the shape of the loop is the
// part worth copying:
//
//     Start -> Ready -> { AdvanceTo, publish, answer } -> Finish
//
// WHERE YOUR CODE GOES. Four places, each marked `TODO(you)` below -- grep for
// it. Everything else is protocol plumbing and stays as it is:
//
//   1. the instrument and its tick grid        -- constants, below
//   2. the event source                        -- ExampleVenue::Run
//   3. the book each event produces            -- ExampleVenue::Walk
//   4. matching                                -- ExampleVenue::MatchOne
//
// Build it and get one run through end to end before you change anything: a
// venue that worked and then broke tells you far more than one that has never
// worked.
// ============================================================================

#include "cmf_driver.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using cmf_driver::Level;
using cmf_driver::RejectReason;
using cmf_driver::Session;
using cmf_driver::Side;
using cmf_driver::SimTime;

// ----------------------------------------------------------------------------
// TODO(you) #1: the instrument and its tick grid.
//
// These three come from your dataset. The tick size especially: the engine
// rounds every price it sends to a grid of its own, and if the two grids
// disagree then every quote lands on a price your book does not have and
// nothing ever matches. That is why OnGrid() below refuses instead of rounding.
// ----------------------------------------------------------------------------
constexpr const char* kClassCode = "QJSIM";
constexpr const char* kSecCode = "SBER";

/// The grid this venue trades on. Prices are compared in whole ticks and never
/// as doubles: two prices that print the same can differ in their last bit.
constexpr double kTickSize = 0.01;

/// Where the run starts, and how far apart its events are. Real venues take
/// both from a dataset.
///
/// The step is worth a thought. Synchronisation costs one exchange of null
/// messages per lookahead a gap spans, so events spaced far wider than the
/// lookahead make the run crawl: a 10 ms gap at a 1 ms lookahead is ten round
/// trips before anything happens. Real tick data is denser than any sensible
/// lookahead and never notices; a synthetic feed like this one has to be told.
constexpr SimTime kStartTs = 1'000'000'000;
constexpr SimTime kStepNs = 200'000;
constexpr int kEventCount = 200;

[[nodiscard]] long long Ticks(double price) {
    return static_cast<long long>(price / kTickSize + (price < 0 ? -0.5 : 0.5));
}

[[nodiscard]] bool OnGrid(double price) {
    return price > 0.0
        && std::abs(price - static_cast<double>(Ticks(price)) * kTickSize) < kTickSize / 1024.0;
}

/// One order this venue is holding on its book.
struct Working {
    std::uint64_t client_id = 0;
    std::uint64_t order_id = 0;
    Side side = Side::Buy;
    long long price_ticks = 0;
    std::uint64_t leaves = 0;
};

/// A venue: a book that moves, the orders resting against it, and the ids it
/// has issued.
class ExampleVenue {
public:
    explicit ExampleVenue(Session& session) : session_(session) {
        session_.on_new_order = [this](const cmf_driver::NewOrder& order) { OnNewOrder(order); };
        session_.on_cancel = [this](const cmf_driver::Cancel& cancel) { OnCancel(cancel); };
    }

    void Run() {
        session_.Ready(kStartTs);

        SimTime ts = kStartTs;

        // --------------------------------------------------------------------
        // TODO(you) #2: the event source.
        //
        // This counted loop stands in for iterating your dataset. Replace the
        // loop and `ts` with your own events, in NON-DECREASING timestamp
        // order; the `seq` passed to PublishSnapshot below is their number.
        //
        // Keep the body's shape exactly: AdvanceTo, then check EngineFinished,
        // then publish, then match. Each step is there for a reason spelled out
        // where it stands.
        // --------------------------------------------------------------------
        for (int event = 0; event < kEventCount; ++event) {
            // The one call that matters. It blocks while the engine has not
            // allowed us this far, draining orders and announcing where we
            // stand -- so by the time it returns, everything the engine has
            // said about times up to `ts` has already reached our handlers.
            session_.AdvanceTo(ts);

            if (session_.EngineFinished()) {
                break;
            }

            Walk(event);
            session_.PublishSnapshot(static_cast<std::uint64_t>(event) + 1, ts,
                                     kClassCode, kSecCode, bids_, asks_);

            // Anything resting that the new book now crosses.
            MatchAgainstBook(ts);

            ts += kStepNs;
        }

        session_.Finish();
    }

private:
    // ------------------------------------------------------------------------
    // TODO(you) #3: the book.
    //
    // Replace this with the state of YOUR book after the event just read. It is
    // the whole visible book, not a delta, and it has to obey three things the
    // engine relies on: levels best-first (bids descending, asks ascending), at
    // most 64 a side, and a volume of exactly zero meaning "no such level".
    // ------------------------------------------------------------------------

    /// A book that moves on its own. Yours comes from a file.
    void Walk(int event) {
        const double mid = 239.50 + kTickSize * static_cast<double>(event % 20);

        bids_ = {
            {mid - kTickSize, 100},
            {mid - 2 * kTickSize, 250},
        };

        asks_ = {
            {mid + kTickSize, 80},
            {mid + 2 * kTickSize, 40},
        };
    }

    void OnNewOrder(const cmf_driver::NewOrder& order) {
        // The order takes effect at effective_ts, never before: that delay is
        // the latency the run is measured at, and shortening it invents an
        // edge the engine was promised it would not get.
        const SimTime effective = order.effective_ts;

        if (order.class_code != kClassCode || order.sec_code != kSecCode) {
            session_.Reject(order.client_id, effective, RejectReason::UnknownInstrument);
            return;
        }

        // REFUSED, not rounded. If the engine's grid and ours disagree, every
        // quote lands on a price this book does not have and nothing matches;
        // refusing turns the quietest possible failure into a loud one on the
        // first order.
        if (!OnGrid(order.price)) {
            session_.Reject(order.client_id, effective, RejectReason::PriceOffTickGrid);
            return;
        }

        if (order.qty == 0) {
            session_.Reject(order.client_id, effective, RejectReason::InvalidParameters);
            return;
        }

        Working working;
        working.client_id = order.client_id;
        working.order_id = ++next_order_id_;
        working.side = order.side;
        working.price_ticks = Ticks(order.price);
        working.leaves = order.qty;

        // Accepted BEFORE any fill of it: the acceptance is where the id the
        // fill refers to comes from.
        session_.Accept(working.client_id, effective, working.order_id, working.leaves);

        book_.push_back(working);
        MatchOne(book_.back(), effective);
        Reap();
    }

    void OnCancel(const cmf_driver::Cancel& cancel) {
        const SimTime effective = cancel.effective_ts;

        // order_id == 0 is not malformed: on the way out the engine pulls
        // orders it has no id for yet. Answer, do not fall over.
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

    // ------------------------------------------------------------------------
    // TODO(you) #4: matching. This is the exercise.
    //
    // What is here fills anything crossing the touch, in full, instantly. That
    // is the most generous model there is and it flatters a passive quoter:
    // in this world a resting order is filled the moment the market reaches it
    // and never waits behind anyone.
    //
    // A real one walks the levels, respects available volume, and models queue
    // position. The protocol already carries all of that -- send one `Fill` per
    // match event with the quantity that actually traded, and `leaves` says how
    // much of the order is still working. Zero `leaves` ends it, and after that
    // nothing more may be said about that order.
    // ------------------------------------------------------------------------
    void MatchOne(Working& working, SimTime ts) {
        if (working.leaves == 0 || bids_.empty() || asks_.empty()) {
            return;
        }

        const long long best_bid = Ticks(bids_.front().price);
        const long long best_ask = Ticks(asks_.front().price);

        const bool crosses = (working.side == Side::Buy) ? (working.price_ticks >= best_ask)
                                                         : (working.price_ticks <= best_bid);

        if (!crosses) {
            return;
        }

        const long long fill_ticks = (working.side == Side::Buy) ? best_ask : best_bid;
        const std::uint64_t qty = working.leaves;

        working.leaves = 0;

        // trade_id is unique for the whole run: the engine deduplicates fills
        // on it, so a repeat would be counted twice and every position derived
        // from it would be wrong.
        session_.Fill(working.client_id, ts, working.order_id, ++next_trade_id_,
                      static_cast<double>(fill_ticks) * kTickSize, qty, working.leaves);
    }

    /// Nothing more may be said about an order once it is finished, so it goes
    /// off the book the moment it is.
    void Reap() {
        std::erase_if(book_, [](const Working& working) { return working.leaves == 0; });
    }

    Session& session_;

    std::vector<Level> bids_;
    std::vector<Level> asks_;
    std::vector<Working> book_;

    std::uint64_t next_order_id_ = 9000;
    std::uint64_t next_trade_id_ = 70000;
};

} // namespace

int main(int argc, char** argv) {
    try {
        const cmf_driver::Options options = cmf_driver::ParseOptions(argc, argv);

        std::fprintf(stderr, "example venue: listening on %s, feed %u, orders %u, %zu dataset(s)\n",
                     options.host.c_str(), options.feed_port, options.order_port,
                     options.datasets.size());

        for (const std::string& dataset : options.datasets) {
            std::fprintf(stderr, "example venue: dataset %s (ignored by this example)\n",
                         dataset.c_str());
        }

        Session session(options);

        const SimTime lookahead = session.Start();
        std::fprintf(stderr, "example venue: engine declared a lookahead of %llu ns\n",
                     static_cast<unsigned long long>(lookahead));

        ExampleVenue(session).Run();

        std::fprintf(stderr, "example venue: dataset finished, said E on both channels\n");
        return EXIT_SUCCESS;

    } catch (const cmf_driver::ProtocolError& error) {
        std::fprintf(stderr, "example venue: PROTOCOL ERROR: %s\n", error.what());
        return EXIT_FAILURE;

    } catch (const std::exception& error) {
        std::fprintf(stderr, "example venue: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
