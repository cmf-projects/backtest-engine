#include "population/order.hpp"
#include "population/population.hpp"
#include "population/tick.hpp"

#include "fake_tape.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using population::BookPatchKind;
using population::ComponentSpec;
using population::ExecType;
using population::Level;
using population::NewOrder;
using population::OrderBatch;
using population::OrderResult;
using population::Population;
using population::Side;
using population::SlotBookUpdateView;
using population::SlotConfig;
using population::SnapshotEvent;
using population::TapePtr;
using population::TimeInForce;
using population::Trade;
using population::test::FakeTape;

namespace {

SnapshotEvent Ev(std::string symbol, std::uint64_t seq, std::uint64_t ts_ns,
                 std::uint64_t bid_qty) {
    SnapshotEvent e;
    e.seq = seq;
    e.ts_ns = ts_ns;
    e.symbol = std::move(symbol);
    e.bids = {Level{100.0, bid_qty}};
    e.asks = {Level{101.0, 5}};
    return e;
}

TapePtr MakeTape(std::vector<SnapshotEvent> events) {
    return std::make_unique<FakeTape>(std::move(events));
}

std::vector<SlotConfig> OneSlot() {
    return {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = 0.0}};
}

const SlotBookUpdateView* FindBook(const std::vector<SlotBookUpdateView>& books,
                                   const std::string& id) {
    for (const auto& b : books) {
        if (b.component_id == id) {
            return &b;
        }
    }
    return nullptr;
}

}  // namespace

// spot: 100, 300
// fut:  100, 200, 400
// Start → pop_ts = 100; Advance → 200, 300, 400 (merge timeline).
TEST(PopulationBundle, StartAndAdvanceFollowMergedTimeline) {
    std::vector<ComponentSpec> comps;
    comps.push_back(ComponentSpec{
        .component_id = "fut",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("FUT", 1, 100, 10),
            Ev("FUT", 2, 200, 20),
            Ev("FUT", 3, 400, 40),
        }),
    });
    comps.push_back(ComponentSpec{
        .component_id = "spot",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("SPOT", 1, 100, 1),
            Ev("SPOT", 2, 300, 3),
        }),
    });

    Population pop(std::move(comps), OneSlot());
    pop.Start();

    EXPECT_EQ(pop.clock().pop_ts_ns(), 100u);
    EXPECT_EQ(pop.component_count(), 2u);
    ASSERT_NE(pop.FindComponent("fut"), nullptr);
    ASSERT_NE(pop.FindComponent("spot"), nullptr);
    EXPECT_EQ(pop.FindComponent("fut")->window().size(), 1u);
    EXPECT_EQ(pop.FindComponent("spot")->window().size(), 1u);
    EXPECT_EQ(pop.FindComponent("fut")->current().bids[0].qty, 10u);
    EXPECT_EQ(pop.FindComponent("spot")->current().bids[0].qty, 1u);

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 200u);
    // fut updated; spot keep-last — window grows only on update
    EXPECT_EQ(pop.FindComponent("fut")->window().size(), 2u);
    EXPECT_EQ(pop.FindComponent("spot")->window().size(), 1u);
    EXPECT_EQ(pop.FindComponent("fut")->current().bids[0].qty, 20u);
    EXPECT_EQ(pop.FindComponent("spot")->current().bids[0].qty, 1u);

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 300u);
    EXPECT_EQ(pop.FindComponent("spot")->window().size(), 2u);
    EXPECT_EQ(pop.FindComponent("spot")->current().bids[0].qty, 3u);

    ASSERT_TRUE(pop.Advance());
    EXPECT_EQ(pop.clock().pop_ts_ns(), 400u);
    EXPECT_EQ(pop.FindComponent("fut")->current().bids[0].qty, 40u);

    EXPECT_FALSE(pop.Advance());
    EXPECT_TRUE(pop.ended());
}

TEST(PopulationBundle, RejectsEmptyOrTooManyComponents) {
    EXPECT_THROW(Population(std::vector<ComponentSpec>{}, OneSlot()), std::invalid_argument);

    std::vector<ComponentSpec> four;
    for (int i = 0; i < 4; ++i) {
        four.push_back(ComponentSpec{
            .component_id = "c" + std::to_string(i),
            .tick_size = 0.01,
            .tape = MakeTape({Ev("X", 1, 100, 1)}),
        });
    }
    EXPECT_THROW(Population(std::move(four), OneSlot()), std::invalid_argument);
}

TEST(PopulationBundle, ImpactIsPerSlotAndComponent) {
    std::vector<ComponentSpec> comps;
    comps.push_back(ComponentSpec{
        .component_id = "fut",
        .tick_size = 0.01,
        .tape = MakeTape({Ev("FUT", 1, 100, 10)}),
    });
    comps.push_back(ComponentSpec{
        .component_id = "spot",
        .tick_size = 0.01,
        .tape = MakeTape({Ev("SPOT", 1, 100, 1)}),
    });

    Population pop(std::move(comps),
                   {SlotConfig{.slot_id = 1, .md_lag_ns = 0, .delta_ns = 1, .impact_lambda = 0.5}});
    pop.Start();

    ASSERT_NE(pop.SlotImpact(1, "fut"), nullptr);
    ASSERT_NE(pop.SlotImpact(1, "spot"), nullptr);
    EXPECT_EQ(pop.SlotImpact(1, "fut"), pop.SlotImpact(1, "fut"));
    EXPECT_NE(pop.SlotImpact(1, "fut"), pop.SlotImpact(1, "spot"));
    EXPECT_EQ(pop.SlotImpact(1, "missing"), nullptr);
}

// IMPL 3.3: both components present from t=100 → first Tick has both keyframes.
TEST(PopulationBundle, MakeTickSpotAndFutKeyframes) {
    std::vector<ComponentSpec> comps;
    comps.push_back(ComponentSpec{
        .component_id = "fut",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("FUT", 1, 100, 10),
            Ev("FUT", 2, 200, 20),
        }),
    });
    comps.push_back(ComponentSpec{
        .component_id = "spot",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("SPOT", 1, 100, 1),
            Ev("SPOT", 2, 200, 3),
        }),
    });

    Population pop(std::move(comps), OneSlot());
    pop.Start();

    const auto tick0 = pop.MakeTick();
    EXPECT_EQ(tick0.pop_ts_ns, 100u);
    ASSERT_EQ(tick0.slots.size(), 1u);
    ASSERT_EQ(tick0.slots[0].books.size(), 2u);
    const auto* fut0 = FindBook(tick0.slots[0].books, "fut");
    const auto* spot0 = FindBook(tick0.slots[0].books, "spot");
    ASSERT_NE(fut0, nullptr);
    ASSERT_NE(spot0, nullptr);
    EXPECT_EQ(fut0->kind, BookPatchKind::Keyframe);
    EXPECT_EQ(spot0->kind, BookPatchKind::Keyframe);
    EXPECT_EQ(fut0->bids[0].qty, 10u);
    EXPECT_EQ(spot0->bids[0].qty, 1u);

    ASSERT_TRUE(pop.Advance());
    const auto tick1 = pop.MakeTick();
    EXPECT_EQ(tick1.pop_ts_ns, 200u);
    ASSERT_EQ(tick1.slots.size(), 1u);
    // Both changed → both Diff
    ASSERT_EQ(tick1.slots[0].books.size(), 2u);
    const auto* fut1 = FindBook(tick1.slots[0].books, "fut");
    const auto* spot1 = FindBook(tick1.slots[0].books, "spot");
    ASSERT_NE(fut1, nullptr);
    ASSERT_NE(spot1, nullptr);
    EXPECT_EQ(fut1->kind, BookPatchKind::Diff);
    EXPECT_EQ(spot1->kind, BookPatchKind::Diff);
    EXPECT_EQ(fut1->bids[0].qty, 20u);
    EXPECT_EQ(spot1->bids[0].qty, 3u);
}

// IMPL 3.3: fut-only until spot's first event — no empty stub for missing component.
//
//   spot:      200
//   fut:  100, 200
TEST(PopulationBundle, MakeTickOnlyFutUntilSpotStarts) {
    std::vector<ComponentSpec> comps;
    comps.push_back(ComponentSpec{
        .component_id = "fut",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("FUT", 1, 100, 10),
            Ev("FUT", 2, 200, 20),
        }),
    });
    comps.push_back(ComponentSpec{
        .component_id = "spot",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("SPOT", 1, 200, 5),
        }),
    });

    Population pop(std::move(comps), OneSlot());
    pop.Start();

    const auto tick0 = pop.MakeTick();
    EXPECT_EQ(tick0.pop_ts_ns, 100u);
    ASSERT_EQ(tick0.slots.size(), 1u);
    ASSERT_EQ(tick0.slots[0].books.size(), 1u);
    EXPECT_EQ(tick0.slots[0].books[0].component_id, "fut");
    EXPECT_EQ(tick0.slots[0].books[0].kind, BookPatchKind::Keyframe);
    EXPECT_EQ(FindBook(tick0.slots[0].books, "spot"), nullptr);

    ASSERT_TRUE(pop.Advance());
    const auto tick1 = pop.MakeTick();
    EXPECT_EQ(tick1.pop_ts_ns, 200u);
    ASSERT_EQ(tick1.slots.size(), 1u);
    ASSERT_EQ(tick1.slots[0].books.size(), 2u);
    const auto* fut1 = FindBook(tick1.slots[0].books, "fut");
    const auto* spot1 = FindBook(tick1.slots[0].books, "spot");
    ASSERT_NE(fut1, nullptr);
    ASSERT_NE(spot1, nullptr);
    EXPECT_EQ(fut1->kind, BookPatchKind::Diff);
    EXPECT_EQ(spot1->kind, BookPatchKind::Keyframe);  // first appearance
    EXPECT_EQ(spot1->bids[0].qty, 5u);
}

// IMPL 3.4: unknown / not-yet-present component_id → UnknownComponent (batch not applied).
TEST(PopulationBundle, SubmitRejectsUnknownAndNotPresentComponent) {
    std::vector<ComponentSpec> comps;
    comps.push_back(ComponentSpec{
        .component_id = "fut",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("FUT", 1, 100, 10),
            Ev("FUT", 2, 200, 10),
        }),
    });
    comps.push_back(ComponentSpec{
        .component_id = "spot",
        .tick_size = 0.01,
        .tape = MakeTape({
            Ev("SPOT", 1, 200, 1),  // not present at t=100
        }),
    });

    Population pop(std::move(comps), OneSlot());
    pop.Start();
    (void)pop.MakeTick();  // pop=100; only fut present

    OrderBatch batch;
    batch.orders.push_back(NewOrder{
        .slot_id = 1,
        .client_order_id = 1,
        .component_id = "OTHER",
        .side = Side::Buy,
        .price = 101.0,
        .qty = 1,
        .tif = TimeInForce::Day,
    });
    batch.orders.push_back(NewOrder{
        .slot_id = 1,
        .client_order_id = 2,
        .component_id = "spot",  // known but not present yet
        .side = Side::Buy,
        .price = 101.0,
        .qty = 1,
        .tif = TimeInForce::Day,
    });
    batch.orders.push_back(NewOrder{
        .slot_id = 1,
        .client_order_id = 3,
        .component_id = "fut",
        .side = Side::Buy,
        .price = 101.0,
        .qty = 1,
        .tif = TimeInForce::Day,
    });

    const auto result = pop.Submit(batch);
    ASSERT_EQ(result.order_results.size(), 3u);
    EXPECT_EQ(result.order_results[0], OrderResult::UnknownComponent);
    EXPECT_EQ(result.order_results[1], OrderResult::UnknownComponent);
    EXPECT_EQ(result.order_results[2], OrderResult::Ok);
    EXPECT_EQ(pop.SlotBook(1)->LiveCount(), 0u);  // all-or-nothing
}

// IMPL 3.4: match uses the order's component book only (thin fut ask, fat spot ask).
TEST(PopulationBundle, MatchUsesOwnComponentBook) {
    auto fut_ev = [](std::uint64_t ts, std::uint64_t ask_qty) {
        SnapshotEvent e;
        e.seq = ts;
        e.ts_ns = ts;
        e.symbol = "FUT";
        e.bids = {Level{100.0, 10}};
        e.asks = {Level{101.0, ask_qty}};
        return e;
    };
    auto spot_ev = [](std::uint64_t ts, std::uint64_t ask_qty) {
        SnapshotEvent e;
        e.seq = ts;
        e.ts_ns = ts;
        e.symbol = "SPOT";
        e.bids = {Level{100.0, 10}};
        e.asks = {Level{101.0, ask_qty}};
        return e;
    };

    std::vector<ComponentSpec> comps;
    comps.push_back(ComponentSpec{
        .component_id = "fut",
        .tick_size = 0.01,
        .tape = MakeTape({fut_ev(100, 2), fut_ev(200, 2)}),
    });
    comps.push_back(ComponentSpec{
        .component_id = "spot",
        .tick_size = 0.01,
        .tape = MakeTape({spot_ev(100, 100), spot_ev(200, 100)}),
    });

    Population pop(std::move(comps), OneSlot());
    pop.Start();
    (void)pop.MakeTick();

    OrderBatch batch;
    batch.orders.push_back(NewOrder{
        .slot_id = 1,
        .client_order_id = 7,
        .component_id = "fut",
        .side = Side::Buy,
        .price = 101.0,
        .qty = 5,
        .tif = TimeInForce::Ioc,
    });
    EXPECT_EQ(pop.Submit(batch).order_results[0], OrderResult::Ok);

    ASSERT_TRUE(pop.Advance());
    const auto tick = pop.MakeTick();
    ASSERT_EQ(tick.slots.size(), 1u);
    ASSERT_EQ(tick.slots[0].trades.size(), 2u);
    EXPECT_EQ(tick.slots[0].trades[0].exec_type, ExecType::Fill);
    EXPECT_EQ(tick.slots[0].trades[0].component_id, "fut");
    EXPECT_EQ(tick.slots[0].trades[0].qty, 2u);  // fut ask only, not spot's 100
    EXPECT_EQ(tick.slots[0].trades[1].exec_type, ExecType::Cancel);
    EXPECT_EQ(tick.slots[0].trades[1].component_id, "fut");
}

namespace {

struct TimedTrade {
    std::uint64_t pop_ts_ns = 0;
    Trade trade;
};

bool SameTrade(const Trade& a, const Trade& b) {
    return a.client_order_id == b.client_order_id && a.exec_type == b.exec_type &&
           a.component_id == b.component_id && a.qty == b.qty && a.leaves == b.leaves &&
           a.price == b.price;
}

SnapshotEvent FutAsk(std::uint64_t ts, std::uint64_t ask_qty) {
    SnapshotEvent e;
    e.seq = ts;
    e.ts_ns = ts;
    e.symbol = "FUT";
    e.bids = {Level{100.0, 10}};
    e.asks = {Level{101.0, ask_qty}};
    return e;
}

SnapshotEvent SpotAsk(std::uint64_t ts, std::uint64_t ask_qty) {
    SnapshotEvent e;
    e.seq = ts;
    e.ts_ns = ts;
    e.symbol = "SPOT";
    e.bids = {Level{100.0, 10}};
    e.asks = {Level{101.0, ask_qty}};
    return e;
}

void CollectTrades(std::uint64_t pop_ts_ns, const std::vector<Trade>& trades,
                   std::vector<TimedTrade>& out) {
    for (const Trade& t : trades) {
        out.push_back(TimedTrade{.pop_ts_ns = pop_ts_ns, .trade = t});
    }
}

// One merge clock: spot 100/300, fut 100/200/400; orders on both → fills after Δ.
std::vector<TimedTrade> RunDeterminismScenario() {
    std::vector<TimedTrade> out;

    std::vector<ComponentSpec> comps;
    comps.push_back(ComponentSpec{
        .component_id = "fut",
        .tick_size = 0.01,
        .tape = MakeTape({FutAsk(100, 5), FutAsk(200, 5), FutAsk(400, 5)}),
    });
    comps.push_back(ComponentSpec{
        .component_id = "spot",
        .tick_size = 0.01,
        .tape = MakeTape({SpotAsk(100, 4), SpotAsk(300, 4)}),
    });

    Population pop(std::move(comps), OneSlot());  // delta_ns = 1
    pop.Start();
    (void)pop.MakeTick();  // pop=100

    OrderBatch batch;
    batch.orders.push_back(NewOrder{1, 1, "fut", Side::Buy, 101.0, 3, TimeInForce::Day});
    batch.orders.push_back(NewOrder{1, 2, "spot", Side::Buy, 101.0, 2, TimeInForce::Day});
    const auto submitted = pop.Submit(batch);
    EXPECT_EQ(submitted.order_results[0], OrderResult::Ok);
    EXPECT_EQ(submitted.order_results[1], OrderResult::Ok);

    // Merge advances: 200 (fut), 300 (spot), 400 (fut).
    while (pop.Advance()) {
        const auto tick = pop.MakeTick();
        EXPECT_EQ(tick.slots.size(), 1u);
        if (tick.slots.size() != 1) {
            return out;
        }
        CollectTrades(tick.pop_ts_ns, tick.slots[0].trades, out);
    }
    return out;
}

}  // namespace

// IMPL 3.5: one pop_ts / Advance drives merge; two identical runs → same trades.
TEST(PopulationBundle, TwoRunsSameTrades) {
    const std::vector<TimedTrade> a = RunDeterminismScenario();
    const std::vector<TimedTrade> b = RunDeterminismScenario();

    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].pop_ts_ns, b[i].pop_ts_ns) << "i=" << i;
        EXPECT_TRUE(SameTrade(a[i].trade, b[i].trade)) << "i=" << i;
    }

    // Sanity: both components filled on the shared merge timeline.
    bool saw_fut = false;
    bool saw_spot = false;
    for (const TimedTrade& t : a) {
        if (t.trade.exec_type != ExecType::Fill) {
            continue;
        }
        if (t.trade.component_id == "fut") {
            saw_fut = true;
            EXPECT_EQ(t.pop_ts_ns, 200u);
        }
        if (t.trade.component_id == "spot") {
            saw_spot = true;
            EXPECT_EQ(t.pop_ts_ns, 200u);  // effective at 101; first match chance at 200
        }
    }
    EXPECT_TRUE(saw_fut);
    EXPECT_TRUE(saw_spot);
}

