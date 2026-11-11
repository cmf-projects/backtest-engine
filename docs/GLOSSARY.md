# Population venue glossary

Terms for the multi-strategy population venue path (UDS + SPSC).

## Population model

| Term | Meaning |
| --- | --- |
| **Population** | Group of strategies that share **one** tape clock `pop_ts`, one `SharedCursor`, and one `ComponentBundle`. Strategies do **not** share a trading book: no competition for liquidity, no visibility of each other's orders. |
| **Trading Engine** | External process that keeps **one** long-lived **unix domain socket** to the venue (full duplex control). Creates populations on that socket (`population_id`); each successful create gets **two SPSC** queues for the tick protocol. `SessionEnded` also on the same UDS; ending a pop tears down only its SPSC, not the control socket. Quotas later. **gRPC is not used.** |
| **SPSC pair** | Two single-producer single-consumer queues for one population run after Open. Out = market/data from venue; in = requests from engine. Hot path; no gRPC runtime. |
| **StrategySlot** | One participant: `slot_id`, **own** resting orders, **own** `ImpactState` / effective book, own `md_lag` / `Δ`. Same `pop_ts` as the population. Not an engine `cl_id`. |
| **pop_ts** | Single logical time for the population (match clock / true tape time). All slots advance together. Moves only on Trading Engine **Advance** (pull via SPSC). Advancing may drop `TapeWindow` prefix / move `SnapshotFloor`. |
| **SharedCursor** | Population tape control: where the file is materialized and how far `TapeWindow` is filled. Not a CMF `H` message. |
| **SnapshotFloor** | Oldest timestamp kept materialized/reconstructible in `TapeWindow`: `pop_ts - max_i(md_lag_i)` (enough history for the stalest view). As `pop_ts` moves, the floor advances and prefix deltas are dropped. |
| **TapeWindow** | Shared in-memory chain: snapshot at `SnapshotFloor` plus **BookDelta**s up through `pop_ts` (and prefetch). One window for the whole population — not per slot. |
| **BookDelta** | Incremental patch for one tape step (e.g. 100→101). Rebuild file book at a tick by applying deltas from the floor snapshot. Do **not** call this an “offset”. |
| **tape_stride** | Open knobs: how many consecutive dataset events fold into one Tick/Advance. `0`/`1` = every event (P0). `N > 1` = coalesce for speed; `pop_ts` becomes the last folded event’s `ts`. Not price `tick_size`. |
| **ReadWatermark** | How far the dataset has been parsed into `TapeWindow` (may prefetch slightly ahead of `pop_ts`). |
| **md_lag** | Per-slot view staleness: at `pop_ts = t`, strategy sees file/effective book at `t - md_lag`. |
| **Δ (order delay)** | Per-slot order delay: decision at `t_seen` → matchable at `t_seen + Δ`. Same role as CMF lookahead. **`Δ = 0` is illegal** (reject); minimum legal `Δ` is 1 (logical time unit) until raised to tape step. |
| **H_strat** | How far the **population** may advance `pop_ts` in one scheduler quantum before yield (fairness / batching). All slots walk that span together. |
| **Visibility model D** | Canonical path: every tape update may be observed, but lagged by `md_lag`; orders take effect after `Δ`. |
| **Visibility model B** | D with `md_lag = 0`; `Δ` still configurable. CMF adapter ≈ B. |
| **ComponentBundle** | Up to **three** tapes on one `pop_ts`: **spot**, **futures**, **option** (fewer if a component is absent — do not invent empty books). Each component has a unique **`component_id`** plus `AssetRole`. Merged by `ts_ns`; component with no event at `t` keeps last book. `Population` accepts 1…3 components (IMPL 3.2). |
| **ImpactState** | Per-slot map of liquidity eaten by that slot’s fills, with exponential recovery toward the **file** book. Makes effective books differ across slots. Per-slot config is **`λ` only**: `λ = 0` means impact off (no eaten/recovery); `λ > 0` enables recovery. |
| **Effective book** | Per-slot view/match surface: file book (at the relevant ts) minus that slot’s `ImpactState`, plus that slot’s resting orders only. Match uses this inside the venue. On the wire: `shared_book` ⊕ per-slot `overlay`. |
| **shared_book** | One book update per Tick for all slots (keyframe or diff of the common/file view). Not duplicated per slot. |
| **overlay** | Per-slot levels where effective differs from `shared_book` (resting/impact). Absolute qty; `0` removes; empty = identical to shared. |
| **BookDiff (wire)** | Changed levels with **absolute** qty (“now this much at price”), not ±delta; `0` removes the level (same rule as CMF `I`). Used for `shared_book` diffs and for `overlay`. |
| **Trades (exec field)** | Separate field on each tick: fills/trades for that slot this step. Strategy uses this for margin/turnover/PnL — not inferred from book diff. |
| **client_order_id** | Client-assigned order key for the population UDS/SPSC path. Lifetime id of the order: Order, fills, Cancel, and rejects all use the same id. Book key is `(slot_id, client_order_id)`. No separate venue `order_id` on this path (unlike CMF `ord_id`). **Contract:** within one population, ids are assigned by the Engine as **dense consecutive `uint64` starting at 1 and strictly increasing** (no gaps, no reuse while live; after terminal — only a new higher id). Enables dense per-pop indexing later. |

**Removed from the population path:** per-slot `virtual_ts` desync, `HorizonOffset` catch-up, `argmax(offset)` leader. Timing differences between strategies are **`md_lag` and `Δ` only**; trading differences are orders + impact.

## Visibility and delays (model D)

At population time `t`:

```
view_ts_i      = t - md_lag_i       # what slot i may see
t_seen         = t                  # decision time on this step
effective_ts_i = t_seen + Δ_i       # when slot i's new order is matchable

B = D with md_lag = 0
```

Example: `md_lag = 3`, `Δ = 2`, `pop_ts = 105`:

```
match / true tape:     105
strategy sees:         102
new order matchable:   107
```

No look-ahead: strategy never sees timestamps `> t - md_lag`.

## Shared tape + per-slot state

```
One TapeWindow (file):
  snap@floor ⊕ Δ… → file book at any ts in [floor, pop_ts]

Each slot additionally:
  Working orders, ImpactState
  effective_qty = max(0, file_qty - eaten_slot)

Advance quantum:
  1. Prefetch BookDeltas through planned pop_ts + H_strat (and floor history).
  2. For each step t in (old_pop_ts, new_pop_ts]:
       for each slot i:
         view at t - md_lag_i (file ⊕ impact_i at view_ts)
         accept orders from strategy
         recover impact_i; match due orders (effective_ts ≤ t) vs effective book_i
  3. pop_ts = new_pop_ts; advance SnapshotFloor; drop prefix.
```

File tape is shared; fills on slot A never change file book or slot B’s effective book.

## CMF protocol (do not overload)

| CMF term | Meaning | Not the same as |
| --- | --- | --- |
| **Horizon `H\|ts`** | “Everything stamped ≤ ts is sent on this channel.” Sync promise on feed/order TCP. | `H_strat` / `md_lag` |
| **Lookahead Δ** | Declared order latency from `HELLO`; venue stays strictly below `T_engine + Δ`. | `md_lag` |
| **`cl_id`** | Opaque engine transaction id; echo only. | `slot_id` |
| **`K`** | Account ready / run origin stamp. | Opening a population |

Many populations share one long-lived **UDS** control socket; each create gets its
own **SPSC pair**. Message shapes live in `src/transport/` (no protobuf/gRPC).

## Liquidity recovery

```
effective_qty(px) = max(0, file_qty(px) - eaten(px))

On file step at t_new (population advance), for each slot, for each level
still present in the file book at t_new:
  eaten *= exp(-λ * (t_new - t_last))
  t_last = t_new
If a level disappears from the file book: drop eaten for that key.

A fill on a slot increases only that slot's eaten at the fill price.
```

`λ` (`impact_lambda`) defaults at population level but **may be overridden per slot**
(`λ = 0` disables impact) to A/B how much results depend on recovery.
Units: per nanosecond (or per tape step once stride exists).
