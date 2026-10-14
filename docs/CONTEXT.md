# CMF venue integration

We implement a **Venue** that a **Trading Engine** connects to over two TCP channels.
The Venue reads a local **Dataset** and publishes market data; the Engine’s **Strategy**
sends orders and receives fills.

## Language

### Roles

**Venue**:
The exchange simulator we write (`src/venue/` → binary `backtest`). It owns the book,
matching, and the dataset file. Protocol transport lives in `include/cmf/cmf_driver.hpp`.
_Avoid_: backtest (as a separate component), engine, strategy

**Trading Engine**:
The external process that dials into the Venue. It contains strategy logic and is
not part of this repository. For local tests we use `scripts/simple_strategy.py` as a
stand-in.
_Avoid_: venue, backtest binary

**Strategy**:
Decision logic that lives inside the Trading Engine (or the Python stand-in). It
observes the book and submits orders.
_Avoid_: venue, matcher

### Channels (TCP)

**Feed channel**:
Venue → Engine only. Carries book snapshots (`S`), increments (`I`), horizons (`H`), and end (`E`).
First update per instrument is `S`; later diffs go as `I` (absolute qty, `0` removes a level).
_Avoid_: “dataset input”, “orders”

**Order channel**:
Bidirectional. Engine → Venue: `HELLO`, `N`, `C`, `TIME`, `BYE`. Venue → Engine:
`K`, `A`/`R`/`F`/`X`/`Y`, `H`, `E`.
_Avoid_: feed

### Data

**Dataset**:
A file on disk that only the Venue reads (raw Databento MBO zip, or converted
snapshot `.jsonl`). The Engine never opens it.
_Avoid_: feed channel, “data over TCP”

**MarketEvent**:
One book snapshot ready to publish: sequence, logical time, bid/ask levels.
_Avoid_: MBO message, raw Databento record

**MBO**:
Databento market-by-order records (`A`/`M`/`C`/`R`…). Converted offline into
`MarketEvent` snapshots by `scripts/mbo_to_snapshots.py`.
_Avoid_: feed channel message `S`

### Time

**Logical time**:
Nanosecond simulation clock carried on messages (`ts`, `ts_seen`, `TIME`).
_Avoid_: wall clock

**Lookahead (Δ)**:
Declared order latency from `HELLO`. The Venue must stay strictly below
`EngineTime + Δ`.

## Who sends what

```
Dataset file ──read──► Venue ──Feed TCP (S)──► Engine / Strategy
                         ▲                         │
                         └──── Order TCP (N/C) ─────┘
                         ─── Order TCP (A/F) ───────►
```

## Layout (Pitchfork-style)

```
include/cmf/          public protocol driver
src/config/           TOML load (private)
src/venue/            venue logic (private)
apps/backtest/        entrypoint + venue `config.toml` → binary `backtest`
scripts/              offline converters + strategy stand-in (`strategy.toml`)
docs/                 TASK, CONTEXT
```

## Work sequence (agreed)

1. **Dataset source** — Venue reads converted `.jsonl` (`JsonlFeed`) ✅
2. **Instrument + tick** — from venue `apps/backtest/config.toml` ✅
   Strategy stand-in has its own `scripts/strategy.toml` (host/ports/HELLO/qty/tif).
3. **Matching**
   - ✅ step 1: partial fill vs touch volume (`min(leaves, level.qty)`)
   - ✅ step 2: walk deeper opposite levels in one `MatchOne` while price crosses
   - ✅ step 3: TIF — DAY rests leftover; IOC matches then `X` leftover; FOK all-or-nothing (`R` if short)

Offline helper: convert MBO zip → `.jsonl` via `scripts/mbo_to_snapshots.py`.
