# Population venue — agent context

Orientation for agents working in this repo. **Semantics** → [`POPULATION_TZ.md`](POPULATION_TZ.md).  
**What to code next** → [`POPULATION_IMPL.md`](POPULATION_IMPL.md).  
**Terms** → [`GLOSSARY.md`](GLOSSARY.md). Architecture draft: [`POPULATION_PLAN.md`](POPULATION_PLAN.md).

CMF TCP (`backtest` / `cmf_driver` / two TCP ports) is **gone**. Only the population UDS path remains.

---

## Roles

**Venue** — this repo. Binary `population_venue`: listens on a Unix domain socket, owns datasets, `Population` matching/impact, SPSC pairs, `SessionEnded`.  
_Avoid_: calling it “engine”, “strategy”, or the old `backtest` binary.

**Trading Engine** — external process (not in this repo). One long-lived UDS to the venue; `CreateQueues` per population; tick protocol on that pop’s SPSC pair.  
_Avoid_: “venue”.

**Strategy / genome** — lives inside the Trading Engine. Sees Tick views; submits OrderBatch; does not own the file book.

---

## Transport (canonical)

```
Engine ──UDS──► Venue          CreateQueues / CreateAck / SessionEnded
Engine ◄─SPSC out── Venue      OpenAck, Tick, OrdersAck, End, Error, …
Engine ──SPSC in──► Venue      Open, OrderBatch, Advance, Close
```

- One UDS connection → many populations (each Create → thread + own SPSC pair).
- Hot path is SPSC; UDS is control + lifecycle only.
- Wire = binary framing in `src/transport/` (no protobuf/gRPC).

Pull model: venue advances `pop_ts` only on Engine **Advance**.

---

## Layout

```
apps/population_venue/      main → LoadConfig + App::Run
apps/population_smoke/      in-process Open→Tick→Advance→End
options/                    TOML per entrypoint (e.g. population_venue.toml)
src/population_venue/       config/ + app/ + shutdown/ (listen, spawn sessions, Ctrl+C)
src/transport/              UDS, QueueRegistry, SPSC bus, PopulationSession
src/population/             core (1…3 components via MergedTape)
src/dataset/                JSONL → Tape
testdata/population/        smoke JSONL
scripts/mbo_to_snapshots.py offline MBO → snapshot JSONL
docs/                       TZ, IMPL, PLAN, GLOSSARY, this file
third_party/ipc.h           unused reference SPMC (own SPSC already)
```

Configs: `options/*.toml` per entrypoint (default `options/population_venue.toml`).  
Override via `CONFIG` / `POPULATION_UDS` / `POPULATION_DRAIN_MS`.

---

## Invariants agents must not break

- Strategies **do not** compete for liquidity or see each other’s orders (per-slot effective book + impact).
- One **`pop_ts`** per population; timing knobs are **`md_lag`** and **`Δ` only** (`Δ ≥ 1`).
- Bundle contract: up to **3** components (spot / futures / option); missing component → fewer books, not empty stubs. **Population accepts 1…3** (IMPL 3.2); MakeTick books for late components / match polish → 3.3–3.4.
- Fatal protocol / bad Open / bad OrderBatch → Error + `SessionEnded{fatal}` + drain Close|timeout (recoverable OrdersAck is later).

---

## Make / verify

```bash
make build
make test
make population-smoke
make population          # listen; default UDS /tmp/cmf-population.sock
```

Next coding milestone: see IMPL (**5.2–5.3** логер plain/JSON; веха 4 + 5.1 done).
