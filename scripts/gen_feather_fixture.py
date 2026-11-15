#!/usr/bin/env python3
"""
Write a tiny Databento bbo-1m Feather delivery tree for tests/dataset.

Same column layout and sidecars as the real deliveries under data/:
  <out>/<YYYYMMDD>/<JOB>/{manifest.json, source-metadata.json, part-00000.feather}

Needs pyarrow:
  uv run --no-project --with pyarrow scripts/gen_feather_fixture.py
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.feather as feather

PRICE_SCALE = 1_000_000_000
UNDEF_PRICE = 2**63 - 1
MINUTE = 60 * 1_000_000_000
DAY = 86_400 * 1_000_000_000
T0 = 1_735_689_600 * 1_000_000_000  # 2025-01-01T00:00:00Z

SCHEMA = pa.schema(
    [
        ("ts_recv", pa.uint64(), False),
        ("ts_event", pa.uint64(), False),
        ("rtype", pa.uint8(), False),
        ("publisher_id", pa.uint16(), False),
        ("instrument_id", pa.uint32(), False),
        ("side", pa.uint8(), False),
        ("price", pa.int64(), False),
        ("size", pa.uint32(), False),
        ("flags", pa.uint8(), False),
        ("sequence", pa.uint32(), False),
        ("bid_px_00", pa.int64(), False),
        ("ask_px_00", pa.int64(), False),
        ("bid_sz_00", pa.uint32(), False),
        ("ask_sz_00", pa.uint32(), False),
        ("bid_ct_00", pa.uint32(), False),
        ("ask_ct_00", pa.uint32(), False),
        ("source_row", pa.uint64(), False),
    ],
    metadata={"schema_name": "databento_bbo_1m", "price_scale": str(PRICE_SCALE)},
)


def px(value: float | None) -> int:
    return UNDEF_PRICE if value is None else round(value * PRICE_SCALE)


# (ts_recv, instrument_id, bid, bid_sz, ask, ask_sz); None price = UNDEF_PRICE.
DAYS = {
    "20250101": {
        "compression": "lz4",
        "symbols": {"MGCZ5": 100, "SILZ5": 200, "MGCZ5-MGCG6": 300},
        "rows": [
            (T0 + 1 * MINUTE, 100, 4230.1, 3, 4230.3, 1),
            (T0 + 1 * MINUTE, 200, 53.835, 2, 53.845, 1),
            (T0 + 2 * MINUTE, 200, 53.840, 1, 53.850, 4),
            (T0 + 2 * MINUTE, 100, 4230.2, 5, None, 0),
            (T0 + 3 * MINUTE, 300, -12.3, 1, -12.1, 2),
        ],
    },
    "20250102": {
        "compression": "lz4",
        "symbols": {"SILZ5": 200},
        "rows": [
            (T0 + DAY + 1 * MINUTE, 200, 54.000, 1, 54.010, 1),
        ],
    },
    "20250103": {
        "compression": "uncompressed",
        # Same contract, new instrument_id on this day.
        "symbols": {"MGCZ5": 101, "SILZ5": 200},
        "rows": [
            (T0 + 2 * DAY + 1 * MINUTE, 101, None, 0, 4240.0, 2),
            (T0 + 2 * DAY + 1 * MINUTE, 200, 54.100, 3, 54.110, 1),
        ],
    },
}


def write_day(out: Path, day: str, spec: dict) -> None:
    job = out / day / "GLBX-TEST"
    job.mkdir(parents=True, exist_ok=True)
    rows = spec["rows"]
    cols = {name: [] for name in SCHEMA.names}
    for i, (ts, iid, bid, bid_sz, ask, ask_sz) in enumerate(rows, start=1):
        cols["ts_recv"].append(ts)
        cols["ts_event"].append(ts - 1)
        cols["rtype"].append(196)
        cols["publisher_id"].append(1)
        cols["instrument_id"].append(iid)
        cols["side"].append(ord("N"))
        cols["price"].append(px(bid if bid is not None else ask))
        cols["size"].append(1)
        cols["flags"].append(128)
        cols["sequence"].append(i)
        cols["bid_px_00"].append(px(bid))
        cols["ask_px_00"].append(px(ask))
        cols["bid_sz_00"].append(bid_sz)
        cols["ask_sz_00"].append(ask_sz)
        cols["bid_ct_00"].append(1 if bid_sz else 0)
        cols["ask_ct_00"].append(1 if ask_sz else 0)
        cols["source_row"].append(i)
    table = pa.table(cols, schema=SCHEMA)
    feather.write_feather(table, job / "part-00000.feather", compression=spec["compression"])

    manifest = {
        "compression": spec["compression"],
        "interval_seconds": 60,
        "market": "GLBX.MDP3",
        "parts": [{"filename": "part-00000.feather", "rows": len(rows)}],
        "price_scale": PRICE_SCALE,
        "schema_name": "databento_bbo_1m",
        "schema_version": 1,
        "status": "complete",
    }
    metadata = {
        "dataset": "GLBX.MDP3",
        "schema": "bbo-1m",
        "stype_out": "instrument_id",
        "mappings": [
            {"raw_symbol": sym, "intervals": [{"symbol": str(iid)}]}
            for sym, iid in spec["symbols"].items()
        ],
    }
    (job / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (job / "source-metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=Path("testdata/dataset/bbo_1m"))
    args = ap.parse_args()
    for day, spec in DAYS.items():
        write_day(args.out, day, spec)
    print(f"# wrote {len(DAYS)} days to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
