# backtest-engine — CMF venue

Local exchange simulator (**venue**) that reads a market-data dataset and speaks the
CMF TCP protocol. A trading engine (or the Python stand-in) connects on two ports:
feed (book) and orders.

Protocol / assignment text: [`docs/TASK.md`](docs/TASK.md).  
Domain terms and layout: [`docs/CONTEXT.md`](docs/CONTEXT.md).  
Starter reference: [`examples/example_venue.cc`](examples/example_venue.cc), PDF in [`files/`](files/).

## Requirements

- CMake ≥ 3.14, C++20 compiler
- Python 3.11+ (strategy stand-in uses `tomllib`)
- Network to fetch [asio](https://github.com/chriskohlhoff/asio) and [toml++](https://github.com/marzer/tomlplusplus) on first configure

## Quick start

```bash
make build          # cmake + compile → build/backtest
make run            # start venue, then Python strategy; wait for both
```

Expected smoke: strategy sends a few sells, venue answers `A`/`F`, both sides send `E`.

Separate processes:

```bash
make venue          # CONFIG=apps/backtest/config.toml ./build/backtest
make strategy       # python3 scripts/simple_strategy.py --config scripts/strategy.toml
```

Override configs:

```bash
make venue VENUE_CONFIG=/path/to/venue.toml
make strategy STRATEGY_CONFIG=/path/to/strategy.toml
# or:
CONFIG=/path/to/venue.toml ./build/backtest
```

## Configure

| File | Who | Main knobs |
| --- | --- | --- |
| `apps/backtest/config.toml` | venue | `host`, `feed_port`, `order_port`, `dataset`, `class_code`, `sec_code`, `tick_size` |
| `scripts/strategy.toml` | stand-in | same host/ports/instrument, plus `lookahead_ns`, `interval_ns`, `qty`, `tif` (0=DAY, 1=IOC, 2=FOK), `max_orders` |

`sec_code` must match the `symbol` field in the JSONL (and stay ≤ 31 characters).

## Dataset

Venue reads **converted** snapshot JSONL (one object per line), not raw MBO over TCP.

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

Point `dataset` in the venue TOML at the resulting `.jsonl`.

## Layout

```
apps/backtest/     entrypoint + venue config → binary backtest
include/cmf/       protocol driver (cmf_driver.hpp)
src/venue/         book, matching, JSONL feed
src/config/        TOML loader
scripts/           mbo_to_snapshots.py, simple_strategy.py, strategy.toml
docs/              TASK, CONTEXT
data/converted/    sample JSONL (gitignored)
examples/          upstream starter example_venue.cc
files/             protocol PDF
```

## Make targets

| Target | Action |
| --- | --- |
| `make build` | configure + compile |
| `make venue` | run C++ backtest only |
| `make strategy` | run Python stand-in only (venue must already listen) |
| `make run` | venue in background, then strategy |
| `make clean` | remove `build/` |
| `make help` | list targets |

## Notes

- First book update per instrument is `S`; later updates are `I` diffs (fallback to `S` if >64 changes). A real engine may still ignore `I` — see `docs/TASK.md` §5.2.
- Matching supports DAY / IOC / FOK.
