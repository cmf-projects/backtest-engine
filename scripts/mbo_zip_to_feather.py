#!/usr/bin/env python3
"""
Repack Databento XEUR.EOBI MBO JSON (inside the HW_1 zips) into Feather.

One delivery per day, same tree the venue already walks:

  <out>/{fut|opt}/<YYYYMMDD>/<job>/{manifest.json,source-metadata.json,part-00000.feather}

The part file is Arrow IPC (Feather V2), schema_name databento_mbp: one L2
snapshot per instrument when the visible book changes at F_LAST. Levels are
bid_px_00..63 / ask_px_00..63 (best first). An empty slot is UNDEF_PRICE
(2^63-1) and size 0. Prices are int64 fixed-point (price_scale 1e9).
FeatherBboTape reads this directly.

  uv run --no-project --with pyarrow --with orjson scripts/mbo_zip_to_feather.py
"""

from __future__ import annotations

import argparse
import hashlib
import json
import multiprocessing as mp
import time
import zipfile
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timedelta, timezone
from pathlib import Path

PRICE_SCALE = 1_000_000_000
UNDEF_PRICE = 2**63 - 1
UINT64_MAX = 2**64 - 1
DEFAULT_BATCH = 4096

FUT_ZIP = Path("/home/alexander/Projects/cmf-hw1-options/data/HW_1/XEUR-20260409-HTT6HHLT6R1.zip")
OPT_ZIP = Path("/home/alexander/Projects/cmf-hw1-options/data/HW_1/XEUR-20260409-HJTR7RCAKT1.zip")

F_LAST = 1 << 7
MAX_LEVELS = 64

_BASE_COLUMNS = (
    "ts_recv",
    "ts_event",
    "rtype",
    "publisher_id",
    "instrument_id",
    "side",
    "price",
    "size",
    "flags",
    "sequence",
)


def _level_columns() -> tuple[str, ...]:
    names: list[str] = []
    for i in range(MAX_LEVELS):
        names.append(f"bid_px_{i:02d}")
        names.append(f"ask_px_{i:02d}")
        names.append(f"bid_sz_{i:02d}")
        names.append(f"ask_sz_{i:02d}")
    return tuple(names)


COLUMNS = _BASE_COLUMNS + _level_columns()


def _schema():
    import pyarrow as pa

    fields = [
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
    ]
    for i in range(MAX_LEVELS):
        fields.append((f"bid_px_{i:02d}", pa.int64(), False))
        fields.append((f"ask_px_{i:02d}", pa.int64(), False))
        fields.append((f"bid_sz_{i:02d}", pa.uint32(), False))
        fields.append((f"ask_sz_{i:02d}", pa.uint32(), False))
    return pa.schema(fields)


class _Book:
    """Resting orders for one instrument. levels() is the L2 book."""

    def __init__(self) -> None:
        self.orders: dict[int, tuple[int, int, int]] = {}

    def clear(self) -> None:
        self.orders.clear()

    def apply(self, action: int, order_id: int, side: int, price: int, size: int) -> None:
        if action == ord("R"):
            self.clear()
            return
        if order_id == 0:
            return
        side_ok = side in (ord("B"), ord("A"))
        price_ok = price != UNDEF_PRICE
        if action == ord("A"):
            if not price_ok or size <= 0 or not side_ok:
                self.orders.pop(order_id, None)
                return
            self.orders[order_id] = (side, price, size)
            return
        if action == ord("M"):
            cur = self.orders.get(order_id)
            if cur is None:
                if price_ok and size > 0 and side_ok:
                    self.orders[order_id] = (side, price, size)
                return
            cur_side, cur_px, _ = cur
            self.orders[order_id] = (
                side if side_ok else cur_side,
                price if price_ok else cur_px,
                size,
            )
            if size <= 0:
                self.orders.pop(order_id, None)
            return
        if action == ord("C"):
            cur = self.orders.get(order_id)
            if cur is None:
                return
            cur_side, cur_px, cur_sz = cur
            cancel = size if size > 0 else cur_sz
            left = cur_sz - cancel
            if left <= 0:
                self.orders.pop(order_id, None)
            else:
                self.orders[order_id] = (cur_side, cur_px, left)

    def levels(self) -> tuple[tuple[tuple[int, int], ...], tuple[tuple[int, int], ...]]:
        """Bids best-first, asks best-first, each (price, qty), capped at MAX_LEVELS."""
        bid_qty: dict[int, int] = {}
        ask_qty: dict[int, int] = {}
        for side, price, size in self.orders.values():
            if size <= 0:
                continue
            if side == ord("B"):
                bid_qty[price] = bid_qty.get(price, 0) + size
            elif side == ord("A"):
                ask_qty[price] = ask_qty.get(price, 0) + size
        bids = tuple((px, qty) for px, qty in sorted(bid_qty.items(), reverse=True)[:MAX_LEVELS])
        asks = tuple((px, qty) for px, qty in sorted(ask_qty.items())[:MAX_LEVELS])
        return bids, asks


def _check_u(name: str, value: int, bits: int, line_no: int) -> int:
    if value < 0 or value >= 1 << bits:
        raise ValueError(f"line {line_no}: {name}={value} does not fit uint{bits}")
    return value


def _check_i(name: str, value: int, bits: int, line_no: int) -> int:
    lim = 1 << (bits - 1)
    if value < -lim or value >= lim:
        raise ValueError(f"line {line_no}: {name}={value} does not fit int{bits}")
    return value


def parse_ts_ns(text: str, cache: dict[str, int]) -> int:
    # Databento pretty_ts: 2026-03-09T00:03:00.129732099Z
    if len(text) < 30 or text[10] != "T" or text[19] != "." or not text.endswith("Z"):
        raise ValueError(f"bad timestamp {text!r}")
    day = text[:10]
    base = cache.get(day)
    if base is None:
        y = int(text[0:4])
        mo = int(text[5:7])
        d = int(text[8:10])
        base = int(datetime(y, mo, d, tzinfo=timezone.utc).timestamp()) * 1_000_000_000
        cache[day] = base
    h = int(text[11:13])
    m = int(text[14:16])
    sec = int(text[17:19])
    ns = int(text[20:29])
    return base + ((h * 60 + m) * 60 + sec) * 1_000_000_000 + ns


def parse_px(value: object, line_no: int) -> int:
    if value is None:
        return UNDEF_PRICE
    if isinstance(value, bool):
        raise ValueError(f"line {line_no}: bad price {value!r}")
    if isinstance(value, int):
        return value if abs(value) > 10_000_000 else value * PRICE_SCALE
    if isinstance(value, float):
        return int(round(value * PRICE_SCALE))
    text = str(value).strip()
    if not text or text.lower() == "null":
        return UNDEF_PRICE
    neg = text[0] == "-"
    if neg:
        text = text[1:]
    if "." in text:
        whole, frac = text.split(".", 1)
        digits = (frac + "0000000000")[:10]
        v = int(whole or "0") * PRICE_SCALE + int(digits[:9])
        if digits[9] >= "5":
            v += 1
    else:
        v = int(text) * PRICE_SCALE
    if neg:
        v = -v
    if v >= UNDEF_PRICE or v < -UNDEF_PRICE:
        raise ValueError(f"line {line_no}: price {value!r} out of int64 range")
    return v


def _ascii_code(name: str, value: object, line_no: int) -> int:
    if not isinstance(value, str) or len(value) != 1:
        raise ValueError(f"line {line_no}: {name}={value!r}")
    code = ord(value)
    if code > 127:
        raise ValueError(f"line {line_no}: {name}={value!r}")
    return code


def _empty_cols() -> dict[str, list]:
    return {name: [] for name in COLUMNS}


def _flush_batch(chunks: dict[str, list], cols: dict[str, list], schema) -> int:
    import pyarrow as pa

    n = len(cols["ts_recv"])
    if n == 0:
        return 0
    for name in COLUMNS:
        field = schema.field(name)
        chunks[name].append(pa.array(cols[name], type=field.type))
        cols[name].clear()
    return n


def _write_feather(path: Path, chunks: dict[str, list], schema) -> tuple[int, int]:
    import pyarrow as pa
    import pyarrow.feather as feather

    arrays = {name: pa.chunked_array(chunks[name], type=schema.field(name).type) for name in COLUMNS}
    table = pa.table(arrays, schema=schema)
    # FeatherWriter emits one IPC record batch per aligned chunk.
    feather.write_feather(table, path, compression="lz4")
    n_batches = pa.ipc.open_file(path).num_record_batches
    return table.num_rows, n_batches


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _next_day(yyyymmdd: str) -> str:
    day = datetime.strptime(yyyymmdd, "%Y%m%d").date() + timedelta(days=1)
    return day.isoformat()


def _day_iso(yyyymmdd: str) -> str:
    return f"{yyyymmdd[0:4]}-{yyyymmdd[4:6]}-{yyyymmdd[6:8]}"


def convert_day(task: dict) -> dict:
    import orjson

    t0 = time.perf_counter()
    zip_path = Path(task["zip_path"])
    member = task["member"]
    out_dir = Path(task["out_dir"])
    batch_rows = int(task["batch_rows"])
    day = task["day"]
    manifest_path = out_dir / "manifest.json"
    if manifest_path.is_file() and not task["force"]:
        prev = json.loads(manifest_path.read_text())
        if (
            prev.get("status") == "complete"
            and prev.get("schema_name") == "databento_mbp"
            and prev.get("batch_rows") == batch_rows
        ):
            return {
                "leg": task["leg"],
                "day": day,
                "rows": prev.get("total_rows", 0),
                "batches": prev.get("record_batches", 0),
                "symbols": prev.get("symbol_count", 0),
                "skipped": True,
                "seconds": 0.0,
                "out": str(out_dir),
            }

    schema = _schema()
    cols = _empty_cols()
    chunks: dict[str, list] = {name: [] for name in COLUMNS}
    ts_cache: dict[str, int] = {}
    symbols: dict[str, set[int]] = {}
    books: dict[int, _Book] = {}
    last_levels: dict[int, tuple] = {}
    rows = 0
    first_ts = 0
    last_ts = 0
    batch_sizes: list[int] = []

    def append_book(ts_recv: int, ts_event: int, instrument_id: int, publisher_id: int, levels: tuple) -> None:
        nonlocal rows, first_ts, last_ts
        bids, asks = levels
        cols["ts_recv"].append(ts_recv)
        cols["ts_event"].append(ts_event)
        cols["rtype"].append(196)
        cols["publisher_id"].append(publisher_id)
        cols["instrument_id"].append(instrument_id)
        cols["side"].append(ord("N"))
        cols["price"].append(bids[0][0] if bids else (asks[0][0] if asks else UNDEF_PRICE))
        cols["size"].append(1)
        cols["flags"].append(F_LAST)
        cols["sequence"].append(rows + 1)
        for i in range(MAX_LEVELS):
            if i < len(bids):
                cols[f"bid_px_{i:02d}"].append(bids[i][0])
                cols[f"bid_sz_{i:02d}"].append(bids[i][1])
            else:
                cols[f"bid_px_{i:02d}"].append(UNDEF_PRICE)
                cols[f"bid_sz_{i:02d}"].append(0)
            if i < len(asks):
                cols[f"ask_px_{i:02d}"].append(asks[i][0])
                cols[f"ask_sz_{i:02d}"].append(asks[i][1])
            else:
                cols[f"ask_px_{i:02d}"].append(UNDEF_PRICE)
                cols[f"ask_sz_{i:02d}"].append(0)
        if rows == 0:
            first_ts = ts_recv
        rows += 1
        last_ts = ts_recv
        if len(cols["ts_recv"]) >= batch_rows:
            batch_sizes.append(_flush_batch(chunks, cols, schema))

    with zipfile.ZipFile(zip_path) as zf, zf.open(member) as fh:
        for line_no, raw in enumerate(fh, start=1):
            if not raw.strip():
                continue
            rec = orjson.loads(raw)
            hd = rec["hd"]
            ts_recv = parse_ts_ns(rec["ts_recv"], ts_cache)
            ts_event = parse_ts_ns(hd["ts_event"], ts_cache)
            symbol = rec["symbol"]
            if not isinstance(symbol, str) or not symbol:
                raise ValueError(f"{member}:{line_no}: empty symbol")
            instrument_id = _check_u("instrument_id", int(hd["instrument_id"]), 32, line_no)
            order_id = int(rec["order_id"])
            if order_id < 0 or order_id > UINT64_MAX:
                raise ValueError(f"{member}:{line_no}: order_id={order_id}")
            action = _ascii_code("action", rec["action"], line_no)
            symbols.setdefault(symbol, set()).add(instrument_id)
            if action in (ord("T"), ord("F"), ord("N")):
                continue
            side = _ascii_code("side", rec["side"], line_no)
            price = parse_px(rec["price"], line_no)
            size = _check_u("size", int(rec["size"]), 32, line_no)
            flags = _check_u("flags", int(rec["flags"]), 8, line_no)
            book = books.get(instrument_id)
            if book is None:
                book = _Book()
                books[instrument_id] = book
            book.apply(action, order_id, side, price, size)
            if (flags & F_LAST) == 0:
                continue
            levels = book.levels()
            if last_levels.get(instrument_id) == levels:
                continue
            last_levels[instrument_id] = levels
            publisher_id = _check_u("publisher_id", int(hd["publisher_id"]), 16, line_no)
            append_book(ts_recv, ts_event, instrument_id, publisher_id, levels)

    if cols["ts_recv"]:
        batch_sizes.append(_flush_batch(chunks, cols, schema))

    out_dir.mkdir(parents=True, exist_ok=True)
    part_path = out_dir / "part-00000.feather"
    if rows == 0:
        part_path.unlink(missing_ok=True)
        raise RuntimeError(f"no rows in {member}")

    written, n_batches = _write_feather(part_path, chunks, schema)
    if written != rows or n_batches != len(batch_sizes):
        raise RuntimeError(
            f"{member}: wrote rows={written} batches={n_batches}, expected {rows} / {len(batch_sizes)}"
        )
    if any(n != batch_rows for n in batch_sizes[:-1]) or batch_sizes[-1] > batch_rows:
        raise RuntimeError(f"{member}: batch sizes {batch_sizes[:3]}...{batch_sizes[-1:]}")

    start_date = _day_iso(day)
    end_date = _next_day(day)
    mappings = []
    for symbol, ids in sorted(symbols.items()):
        mappings.append(
            {
                "raw_symbol": symbol,
                "intervals": [
                    {"start_date": start_date, "end_date": end_date, "symbol": str(iid)}
                    for iid in sorted(ids)
                ],
            }
        )
    metadata = {
        "dataset": "XEUR.EOBI",
        "schema": "bbo-1m",
        "stype_out": "instrument_id",
        "leg": task["leg"],
        "mappings": mappings,
    }
    manifest = {
        "compression": "lz4",
        "delivery_job": task["job_id"],
        "leg": task["leg"],
        "market": "XEUR.EOBI",
        "batch_rows": batch_rows,
        "record_batches": n_batches,
        "parts": [
            {
                "filename": part_path.name,
                "rows": rows,
                "record_batches": n_batches,
                "first_ts_recv": first_ts,
                "last_ts_recv": last_ts,
                "sha256": _sha256(part_path),
            }
        ],
        "price_scale": PRICE_SCALE,
        "undef_price": UNDEF_PRICE,
        "schema_name": "databento_mbp",
        "schema_version": 1,
        "side_encoding": "ASCII uint8",
        "action_encoding": "ASCII uint8",
        "source": "DATABENTO",
        "source_archive": Path(member).name,
        "source_zip": zip_path.name,
        "source_partition_start": start_date,
        "status": "complete",
        "symbol_count": len(symbols),
        "timestamp_unit": "nanoseconds_since_unix_epoch",
        "total_rows": rows,
    }
    (out_dir / "source-metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return {
        "leg": task["leg"],
        "day": day,
        "rows": rows,
        "batches": n_batches,
        "symbols": len(symbols),
        "skipped": False,
        "seconds": time.perf_counter() - t0,
        "out": str(out_dir),
    }


def _job_of(zip_path: Path) -> tuple[str, list[str]]:
    with zipfile.ZipFile(zip_path) as zf:
        meta_name = next(n for n in zf.namelist() if n.endswith("/metadata.json"))
        meta = json.loads(zf.read(meta_name))
        members = sorted(n for n in zf.namelist() if n.endswith(".mbo.json"))
    return meta["job_id"], members


def _day_of(member: str) -> str:
    stem = Path(member).name.replace(".mbo.json", "")
    return stem.rsplit("-", 1)[-1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fut-zip", type=Path, default=FUT_ZIP)
    ap.add_argument("--opt-zip", type=Path, default=OPT_ZIP)
    ap.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "data-xeur",
    )
    ap.add_argument("--batch-rows", type=int, default=DEFAULT_BATCH)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--only-day", action="append", default=[], help="YYYYMMDD, repeatable")
    ap.add_argument("--force", action="store_true", help="Rewrite days that already have a manifest")
    args = ap.parse_args()
    if args.batch_rows <= 0:
        raise SystemExit("--batch-rows must be positive")

    legs = [("fut", args.fut_zip), ("opt", args.opt_zip)]
    for _, path in legs:
        if not path.is_file():
            raise SystemExit(f"zip not found: {path}")

    only = set(args.only_day)
    tasks = []
    for leg, path in legs:
        job_id, members = _job_of(path)
        for member in members:
            day = _day_of(member)
            if only and day not in only:
                continue
            tasks.append(
                {
                    "leg": leg,
                    "zip_path": str(path),
                    "member": member,
                    "job_id": job_id,
                    "day": day,
                    "out_dir": str(args.out / leg / day / job_id),
                    "batch_rows": args.batch_rows,
                    "force": args.force,
                }
            )
    if not tasks:
        raise SystemExit("no day files selected")

    args.out.mkdir(parents=True, exist_ok=True)
    print(f"# {len(tasks)} days, batch_rows={args.batch_rows}, jobs={args.jobs}, out={args.out}", flush=True)
    results = []
    ctx = mp.get_context("spawn")
    workers = max(1, min(args.jobs, len(tasks)))
    with ProcessPoolExecutor(max_workers=workers, mp_context=ctx) as pool:
        futs = {pool.submit(convert_day, task): task for task in tasks}
        for fut in as_completed(futs):
            task = futs[fut]
            try:
                result = fut.result()
            except Exception as exc:
                raise SystemExit(f"{task['leg']} {task['day']}: {exc}") from exc
            results.append(result)
            flag = "skip" if result["skipped"] else "wrote"
            print(
                f"# {flag} {result['leg']} {result['day']} "
                f"rows={result['rows']:,} batches={result['batches']} "
                f"symbols={result['symbols']} {result['seconds']:.1f}s",
                flush=True,
            )

    results.sort(key=lambda r: (r["leg"], r["day"]))
    summary = {
        "batch_rows": args.batch_rows,
        "schema_name": "databento_mbp",
        "price_scale": PRICE_SCALE,
        "days": results,
        "total_rows": sum(r["rows"] for r in results),
    }
    (args.out / "index.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"# total_rows={summary['total_rows']:,} index={args.out / 'index.json'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
