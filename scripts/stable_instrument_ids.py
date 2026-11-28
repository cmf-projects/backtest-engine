#!/usr/bin/env python3
"""Give each raw_symbol one instrument_id for the whole fut or opt tree.

Databento ids are only unique inside a day. The tape unions every id that ever
meant a symbol, so a reused id mixes another contract into that stream and the
timestamps stop increasing. This rewrites source-metadata.json and the
instrument_id column. Rows stay in place.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.feather as feather
import pyarrow.ipc as ipc

ROOT = Path("/home/alexander/Projects/backtest-engine/data-xeur")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def assign(leg: Path) -> dict[str, int]:
    symbols: set[str] = set()
    for meta in leg.rglob("source-metadata.json"):
        data = json.loads(meta.read_text())
        for mapping in data["mappings"]:
            symbols.add(mapping["raw_symbol"])
    return {symbol: i + 1 for i, symbol in enumerate(sorted(symbols))}


def rewrite_day(day_dir: Path, symbol_id: dict[str, int]) -> int:
    meta_path = day_dir / "source-metadata.json"
    data = json.loads(meta_path.read_text())
    old_to_new: dict[int, int] = {}
    for mapping in data["mappings"]:
        new_id = symbol_id[mapping["raw_symbol"]]
        for interval in mapping["intervals"]:
            old_to_new[int(interval["symbol"])] = new_id
            interval["symbol"] = str(new_id)

    part = day_dir / "part-00000.feather"
    table = feather.read_table(part)
    old = table.column("instrument_id").to_numpy(zero_copy_only=False)
    lut = np.zeros(int(old.max()) + 1, dtype=np.uint32)
    for src, dst in old_to_new.items():
        if src >= len(lut):
            grown = np.zeros(src + 1, dtype=np.uint32)
            grown[: len(lut)] = lut
            lut = grown
        lut[src] = np.uint32(dst)
    mapped = lut[old]
    if np.any(mapped == 0):
        missing = int(old[mapped == 0][0])
        raise SystemExit(f"{part}: instrument_id {missing} is not in source-metadata.json")

    index = table.schema.get_field_index("instrument_id")
    table = table.set_column(index, "instrument_id", pa.array(mapped, type=pa.uint32()))
    tmp = part.with_suffix(".feather.tmp")
    feather.write_feather(table, tmp, compression="lz4")
    tmp.replace(part)

    meta_path.write_text(json.dumps(data, indent=2) + "\n")
    manifest_path = day_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    n_batches = ipc.open_file(part).num_record_batches
    manifest["record_batches"] = n_batches
    for entry in manifest["parts"]:
        if entry["filename"] == part.name:
            entry["record_batches"] = n_batches
            entry["sha256"] = sha256(part)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return int(table.num_rows)


def main() -> None:
    total = 0
    for leg_name in ("fut", "opt"):
        leg = ROOT / leg_name
        symbol_id = assign(leg)
        days = sorted({p.parent for p in leg.rglob("part-00000.feather")})
        print(f"{leg_name}: {len(symbol_id)} symbols, {len(days)} days", flush=True)
        for day in days:
            rows = rewrite_day(day, symbol_id)
            total += rows
            print(f"  {day.relative_to(ROOT)} rows={rows}", flush=True)
    print(f"rewritten rows={total}")


if __name__ == "__main__":
    main()
