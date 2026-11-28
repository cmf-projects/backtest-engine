# backtest-engine — population venue

UDS + SPSC population venue: Engine creates populations over a Unix socket; each
run gets an out/in queue pair and a `PopulationSession` tick loop.

Agent orientation: [`docs/CONTEXT.md`](docs/CONTEXT.md).  
Domain: [`docs/POPULATION_TZ.md`](docs/POPULATION_TZ.md),  
plan: [`docs/POPULATION_IMPL.md`](docs/POPULATION_IMPL.md),  
glossary: [`docs/GLOSSARY.md`](docs/GLOSSARY.md).

## Requirements

- CMake ≥ 3.14, C++23 compiler
- Apache Arrow C++ (`pacman -S arrow` / `apt install libarrow-dev`) for Feather datasets
- Network to fetch [toml++](https://github.com/marzer/tomlplusplus), nlohmann/json (and GoogleTest if tests on) on first configure

## Quick start

```bash
make build
make test
make population-smoke    # same-process: Create → shm attach → Open → Tick → Advance → End
make population          # venue listen (default UDS /tmp/cmf-population.sock)
make population-client   # external Engine: take best bid/ask over shm SPSC
```

### Run venue + client

**Smoke (recommended first check)** — one process, shm SPSC attach by CreateAck names:

```bash
make population-smoke
# optional: POPULATION_DATASET=path/to.jsonl POPULATION_UDS=/tmp/my.sock make population-smoke
```

Uses `testdata/population/smoke.jsonl` by default. Prints ticks and exits 0 on success.

**Cross-process** — venue + take-bid/ask client (two terminals, repo cwd so dataset path resolves):

```bash
# terminal 1
make population
# terminal 2
make population-client
# or: ./build/population_client /tmp/cmf-population.sock testdata/population/smoke.jsonl
```

Default socket: `/tmp/cmf-population.sock` (override `POPULATION_UDS` or toml `uds_path`).  
Engine flow: connect UDS → `CreateQueues` → mmap shm names from `CreateAck` → SPSC
`Open` / `Tick` / `OrderBatch` / `Advance` / `Close`.  
`dataset_path` in Open is opened **by the venue** (path must exist on the venue host).

**Feather book (MGCZ5)** — same two terminals, repo cwd. The client opens `data` as three legs (SPOT, FUT, OPT) on one symbol. Step, lag, order delay, and `order_every_ticks` come from `options/population_client.toml` (100 ms step, IOC every 4th tick).

```bash
# terminal 1
make population
# terminal 2
POPULATION_SEC_CODE=MGCZ5 POPULATION_TICK_SIZE=0.1 \
  ./build/population_client /tmp/cmf-population.sock data
```

**Noise bundle (spot / fut / option):** generate white-noise tapes, then point the client at the directory:

```bash
make population-noise                          # → testdata/population/noise/{spot,fut,option}.jsonl
# optional: POPULATION_NOISE_N=10000 make population-noise
make clean && make build CMF_LOG_LEVEL=trace   # venue TRACE for submit/fill
# terminal 1
make population
# terminal 2
POPULATION_DATASET=testdata/population/noise make population-client
# concurrent populations on one UDS:
POPULATION_DATASET=testdata/population/noise CLIENT_POPS=3 make population-client
```

Client summary prints fill sim-delay (`fill_pop_ts - submit_pop_ts`); with `delta_ns=1` and 1 ms tape step that is typically one event step.

**Shutdown:** `SIGINT` / `SIGTERM` (Ctrl+C) stops Accept/Create, finishes live populations with
`SessionEnded{fatal, "venue shutdown"}`, joins session threads, unlinks the UDS, exits 0.

**Logging (spdlog):** level and format are **compile-time** (lower levels are stripped from
the binary via `SPDLOG_ACTIVE_LEVEL`). Rebuild to change them:

```bash
make clean
make build CMF_LOG_LEVEL=debug CMF_LOG_FORMAT=json   # trace|debug|info|warn|error ; plain|json
make population
```

Knobs (runtime): `POPULATION_UDS`, `POPULATION_DRAIN_MS`, `CONFIG=options/population_venue.toml`.

## Configure

TOML configs for entrypoints live under `options/`:

| File | Who | Main knobs |
| --- | --- | --- |
| `options/population_venue.toml` | `population_venue` | `uds_path`, `drain_timeout_ms` |

## Dataset

The venue picks the adapter from `dataset_path` (`dataset::MakeTape`):

- `*.jsonl` → converted snapshot JSONL (below);
- `*.feather` or a directory → Databento **bbo-1m** Feather deliveries
  (`<day>/<job>/{manifest.json, source-metadata.json, part-*.feather}`).
  The path can be one part, one day, or the whole `data/` root (days replayed in order,
  days without the symbol skipped). `sec_code` = Databento `raw_symbol` (e.g. `MGCZ5`);
  one snapshot per minute with best bid/ask as a single level.

```bash
# MGC Dec-25 over all days in data/ (MGC tick 0.1, SIL tick 0.005)
POPULATION_SEC_CODE=MGCZ5 POPULATION_TICK_SIZE=0.1 make population-smoke POPULATION_DATASET=data
POPULATION_SEC_CODE=SILZ5 POPULATION_TICK_SIZE=0.005 make population-client POPULATION_DATASET=data
```

Snapshot JSONL (one object per line):

Example row:

```json
{"seq":1,"ts_ns":...,"symbol":"FCEU SI 20260615 PS","bids":[{"price":1.1722,"qty":20}],"asks":[...]}
```

Convert from a Databento-style MBO zip / `.mbo.json`:

```bash
python3 scripts/mbo_to_snapshots.py data/dataset/XEUR-….zip \
  -o data/converted/fceu_20260408 \
  --top 2 --max-snapshots 1500
```

## Layout

```
apps/population_venue/   UDS population venue → binary population_venue
apps/population_smoke/   in-process population smoke
options/                 TOML configs per entrypoint
src/population_venue/    config parse + App::Run
src/population/          population core
../wire-engine/          protocol frames (cmf_wire)
src/transport/           UDS + SPSC bus + session
src/dataset/             JSONL / Feather (bbo-1m) → Tape
testdata/population/     sample JSONL for smoke
testdata/dataset/        tiny bbo-1m Feather tree (scripts/gen_feather_fixture.py)
scripts/                 mbo_to_snapshots.py, gen_feather_fixture.py
docs/                    CONTEXT (agents), POPULATION_*, GLOSSARY
```

## Make targets

| Target | Action |
| --- | --- |
| `make build` | configure + compile |
| `make population` | run population UDS venue |
| `make population-smoke` | Open→Tick→Advance→End smoke |
| `make test` | build + ctest |
| `make clean` | remove `build/` |
| `make help` | list targets |
