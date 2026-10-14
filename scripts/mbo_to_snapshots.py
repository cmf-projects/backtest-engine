#!/usr/bin/env python3
"""
Convert Databento XEUR.EOBI MBO JSON (NDJSON, possibly inside a zip) into
book snapshots that the venue can replay later.

Output (JSONL), one snapshot per line:
  {"seq":1,"ts_ns":1775...,"symbol":"FCEU SI 20260615 PS",
   "bids":[{"price":1.1722,"qty":20},...],
   "asks":[{"price":1.1723,"qty":10},...]}

Book rules (from Databento docs):
  A = add/replace order by order_id
  M = modify price/size
  C = cancel size (remove when remaining <= 0)
  R = clear all resting orders for the instrument
  T/F/N = ignore for the book

A snapshot is emitted when flags has F_LAST (bit 7 = 128) for a tracked
instrument — end of an atomic event on that book.
"""

from __future__ import annotations

import argparse
import json
import sys
import zipfile
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Optional, Tuple


F_LAST = 1 << 7
MAX_LEVELS = 64


@dataclass
class Order:
    side: str  # "B" or "A"
    price: float
    size: int


@dataclass
class Book:
    orders: Dict[str, Order] = field(default_factory=dict)

    def clear(self) -> None:
        self.orders.clear()

    def apply(self, action: str, order_id: str, side: str, price: Optional[float], size: int) -> None:
        if action == "R":
            self.clear()
            return

        if not order_id or order_id == "0":
            return

        if action == "A":
            if price is None or size <= 0 or side not in ("B", "A"):
                self.orders.pop(order_id, None)
                return
            self.orders[order_id] = Order(side=side, price=price, size=size)
            return

        if action == "M":
            if order_id not in self.orders:
                if price is not None and size > 0 and side in ("B", "A"):
                    self.orders[order_id] = Order(side=side, price=price, size=size)
                return
            order = self.orders[order_id]
            if price is not None:
                order.price = price
            if size >= 0:
                order.size = size
            if side in ("B", "A"):
                order.side = side
            if order.size <= 0:
                self.orders.pop(order_id, None)
            return

        if action == "C":
            order = self.orders.get(order_id)
            if order is None:
                return
            cancel_qty = size if size > 0 else order.size
            order.size -= cancel_qty
            if order.size <= 0:
                self.orders.pop(order_id, None)
            return

    def levels(self) -> Tuple[List[dict], List[dict]]:
        bid_qty: Dict[float, int] = defaultdict(int)
        ask_qty: Dict[float, int] = defaultdict(int)
        for order in self.orders.values():
            if order.size <= 0:
                continue
            if order.side == "B":
                bid_qty[order.price] += order.size
            elif order.side == "A":
                ask_qty[order.price] += order.size

        bids = [
            {"price": px, "qty": qty}
            for px, qty in sorted(bid_qty.items(), key=lambda x: -x[0])[:MAX_LEVELS]
            if qty > 0
        ]
        asks = [
            {"price": px, "qty": qty}
            for px, qty in sorted(ask_qty.items(), key=lambda x: x[0])[:MAX_LEVELS]
            if qty > 0
        ]
        return bids, asks


def parse_ts_ns(value: object) -> int:
    """ISO-8601 pretty timestamp or integer nanoseconds -> uint64 ns."""
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        raise ValueError(f"unsupported timestamp: {value!r}")
    if value.isdigit():
        return int(value)

    text = value
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    # Support up to nanoseconds: datetime only parses microseconds, keep remainder.
    if "." in text:
        head, frac_and_tz = text.split(".", 1)
        frac = ""
        tz = "+00:00"
        for i, ch in enumerate(frac_and_tz):
            if ch.isdigit():
                frac += ch
            else:
                tz = frac_and_tz[i:]
                break
        frac_us = (frac + "000000")[:6]
        extra_ns = int((frac + "000000000")[6:9])
        dt = datetime.fromisoformat(f"{head}.{frac_us}{tz}")
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        # datetime only keeps microseconds; fold the remaining nanos back in.
        return int(dt.timestamp()) * 1_000_000_000 + dt.microsecond * 1_000 + extra_ns

    dt = datetime.fromisoformat(text)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return int(dt.timestamp()) * 1_000_000_000 + dt.microsecond * 1_000


def parse_price(value: object) -> Optional[float]:
    if value is None or value == "":
        return None
    if isinstance(value, (int, float)):
        # Databento fixed-point: 1 unit = 1e-9. Pretty JSON uses decimals already.
        # Heuristic: huge ints are fixed-point.
        if isinstance(value, int) and abs(value) > 10_000_000:
            return value * 1e-9
        return float(value)
    text = str(value).strip()
    if not text or text.lower() in ("null", "nan"):
        return None
    return float(text)


def list_mbo_members(zf: zipfile.ZipFile, member: Optional[str]) -> List[str]:
    names = sorted(n for n in zf.namelist() if n.endswith(".mbo.json"))
    if member:
        matches = [n for n in names if n.endswith(member) or member in n]
        if not matches:
            raise SystemExit(f"member not found in zip: {member}")
        return matches
    # Without --member, one day only — full zip is multi-GB.
    if not names:
        raise SystemExit("no .mbo.json members in zip")
    print(f"# warning: no --member; using only first day file: {names[0]}", file=sys.stderr)
    return [names[0]]


def iter_mbo_lines(path: Path, member: Optional[str]) -> Iterator[str]:
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as zf:
            for name in list_mbo_members(zf, member):
                print(f"# reading zip member {name}", file=sys.stderr)
                with zf.open(name) as fh:
                    for raw in fh:
                        yield raw.decode("utf-8")
    else:
        with path.open("r", encoding="utf-8") as fh:
            for line in fh:
                yield line


def discover_top_symbols(lines: Iterable[str], top_n: int, limit_scan: int) -> List[str]:
    counts: Dict[str, int] = defaultdict(int)
    n = 0
    for line in lines:
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        sym = rec.get("symbol")
        if sym:
            counts[sym] += 1
        n += 1
        if limit_scan and n >= limit_scan:
            break
    ranked = sorted(counts.items(), key=lambda x: -x[1])
    return [sym for sym, _ in ranked[:top_n]]


def convert(
    lines: Iterable[str],
    symbols: set[str],
    max_snapshots_per_symbol: int,
    depth: int,
) -> Iterator[dict]:
    books: Dict[str, Book] = {sym: Book() for sym in symbols}
    seq = {sym: 0 for sym in symbols}
    done = set()

    for line in lines:
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        sym = rec.get("symbol")
        if sym not in books or sym in done:
            continue

        action = rec.get("action") or ""
        if action in ("T", "F", "N"):
            # Trades/fills do not change the book.
            continue

        ts_raw = rec.get("ts_recv") or (rec.get("hd") or {}).get("ts_event")
        ts_ns = parse_ts_ns(ts_raw)
        side = rec.get("side") or "N"
        price = parse_price(rec.get("price"))
        size = int(rec.get("size") or 0)
        order_id = str(rec.get("order_id") or "0")
        flags = int(rec.get("flags") or 0)

        book = books[sym]
        book.apply(action, order_id, side, price, size)

        if not (flags & F_LAST):
            continue

        seq[sym] += 1
        bids, asks = book.levels()
        if depth < MAX_LEVELS:
            bids = bids[:depth]
            asks = asks[:depth]

        yield {
            "seq": seq[sym],
            "ts_ns": ts_ns,
            "symbol": sym,
            "bids": bids,
            "asks": asks,
        }

        if max_snapshots_per_symbol and seq[sym] >= max_snapshots_per_symbol:
            done.add(sym)
            if done == symbols:
                return


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="Path to .mbo.json or a dataset .zip")
    ap.add_argument(
        "-o",
        "--output",
        type=Path,
        required=True,
        help="Output .jsonl file, or a directory (one file per symbol)",
    )
    ap.add_argument("--member", help="Substring/filename of zip member, e.g. xeur-eobi-20260408.mbo.json")
    ap.add_argument("--symbols", nargs="+", help="Exact symbol strings to keep")
    ap.add_argument("--top", type=int, default=2, help="If --symbols omitted, keep top-N by message count (default 2)")
    ap.add_argument("--scan-limit", type=int, default=500_000, help="Rows to scan when auto-picking symbols")
    ap.add_argument(
        "--max-snapshots",
        type=int,
        default=0,
        help="Max snapshots PER symbol (0 = all)",
    )
    ap.add_argument("--depth", type=int, default=10, help="Max levels per side in output (default 10)")
    args = ap.parse_args()

    if not args.input.exists():
        raise SystemExit(f"input not found: {args.input}")

    symbols = args.symbols
    if not symbols:
        print(f"# discovering top-{args.top} symbols (scan_limit={args.scan_limit})...", file=sys.stderr)
        symbols = discover_top_symbols(iter_mbo_lines(args.input, args.member), args.top, args.scan_limit)
        if not symbols:
            raise SystemExit("no symbols found")
        print("# selected:", *symbols, sep="\n#   ", file=sys.stderr)

    symbol_set = set(symbols)
    # Directory if not explicitly a .jsonl file (one output file per symbol).
    split_dir = args.output.suffix.lower() != ".jsonl"
    if split_dir:
        out_dir = args.output
        out_dir.mkdir(parents=True, exist_ok=True)
        handles = {
            sym: (out_dir / (sym.replace(" ", "_") + ".jsonl")).open("w", encoding="utf-8")
            for sym in symbols
        }
        meta_path = out_dir / "meta.json"
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        single = args.output.open("w", encoding="utf-8")
        handles = {sym: single for sym in symbols}
        meta_path = args.output.with_suffix(args.output.suffix + ".meta.json")

    meta = {
        "source": str(args.input),
        "member": args.member,
        "symbols": symbols,
        "depth": args.depth,
        "max_snapshots_per_symbol": args.max_snapshots,
        "format": "jsonl_snapshots_v1",
        "fields": ["seq", "ts_ns", "symbol", "bids", "asks"],
    }
    meta_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")

    counts = {sym: 0 for sym in symbols}
    try:
        for snap in convert(
            iter_mbo_lines(args.input, args.member),
            symbol_set,
            args.max_snapshots,
            args.depth,
        ):
            handles[snap["symbol"]].write(json.dumps(snap, separators=(",", ":")) + "\n")
            counts[snap["symbol"]] += 1
            total = sum(counts.values())
            if total % 50_000 == 0 and total:
                print(f"# wrote {total} snapshots...", file=sys.stderr)
    finally:
        closed = set()
        for fh in handles.values():
            if id(fh) not in closed:
                fh.close()
                closed.add(id(fh))

    for sym, n in counts.items():
        print(f"# {sym}: {n} snapshots", file=sys.stderr)
    print(f"# meta: {meta_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
