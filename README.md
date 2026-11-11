# backtest-engine — population venue

UDS + SPSC population venue: Engine creates populations over a Unix socket; each
run gets an out/in queue pair and a `PopulationSession` tick loop.

Agent orientation: [`docs/CONTEXT.md`](docs/CONTEXT.md).  
Domain: [`docs/POPULATION_TZ.md`](docs/POPULATION_TZ.md),  
plan: [`docs/POPULATION_IMPL.md`](docs/POPULATION_IMPL.md),  
glossary: [`docs/GLOSSARY.md`](docs/GLOSSARY.md).

## Requirements

- CMake ≥ 3.14, C++20 compiler
- Network to fetch [toml++](https://github.com/marzer/tomlplusplus) (and GoogleTest if tests on) on first configure

## Quick start

```bash
make build
make population-smoke   # same-process Open → Tick → Advance → End
make population         # listen on UDS (default /tmp/cmf-population.sock)
```

Knobs: `POPULATION_UDS`, `POPULATION_DRAIN_MS`, `CONFIG=options/population_venue.toml`.  
Open still carries `dataset_path` (JSONL on the venue host). SPSC queues are **in-process** for now (shm later).

**Shutdown:** `SIGINT` / `SIGTERM` (Ctrl+C) stops Accept/Create, finishes live populations with
`SessionEnded{fatal, "venue shutdown"}`, joins session threads, unlinks the UDS, exits 0.

## Configure

TOML configs for entrypoints live under `options/`:

| File | Who | Main knobs |
| --- | --- | --- |
| `options/population_venue.toml` | `population_venue` | `uds_path`, `drain_timeout_ms` |

## Dataset

Venue reads **converted** snapshot JSONL (one object per line).

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
src/transport/           UDS + SPSC bus + session
src/dataset/             JSONL → Tape
testdata/population/     sample JSONL for smoke
scripts/                 mbo_to_snapshots.py
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
