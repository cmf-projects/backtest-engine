#!/usr/bin/env python3
"""
Minimal fake trading ENGINE + a dumb STRATEGY.

Connects to the venue's feed + order TCP ports, reads book snapshots (S),
and every `interval` of logical time hits the best bid (sell 1 lot at bid).

  ./build/backtest
  python3 scripts/simple_strategy.py --config scripts/strategy.toml
"""

from __future__ import annotations

import argparse
import select
import socket
import sys
import time
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional


@dataclass
class BookTop:
    ts: int = 0
    bid_px: Optional[float] = None
    bid_qty: int = 0


class LocalBook:
    """Apply S / I so the stand-in sees the same book the venue publishes."""

    def __init__(self) -> None:
        self.bids: Dict[float, int] = {}
        self.asks: Dict[float, int] = {}
        self.ts: int = 0

    def apply_snapshot(self, ts: int, levels: List[str], nbid: int, nask: int) -> BookTop:
        self.ts = ts
        self.bids.clear()
        self.asks.clear()
        for i, raw in enumerate(levels[: nbid + nask]):
            px_s, qty_s = raw.split(":")
            px, qty = float(px_s), int(qty_s)
            if i < nbid:
                if qty > 0:
                    self.bids[px] = qty
            else:
                if qty > 0:
                    self.asks[px] = qty
        return self.top()

    def apply_increment(self, ts: int, changes: List[str]) -> BookTop:
        self.ts = ts
        for raw in changes:
            # B:px:qty or A:px:qty
            side, px_s, qty_s = raw.split(":", 2)
            px, qty = float(px_s), int(qty_s)
            book = self.bids if side == "B" else self.asks
            if qty == 0:
                book.pop(px, None)
            else:
                book[px] = qty
        return self.top()

    def top(self) -> BookTop:
        top = BookTop(ts=self.ts)
        if self.bids:
            px = max(self.bids)
            top.bid_px = px
            top.bid_qty = self.bids[px]
        return top


class LineSocket:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.buf = b""
        self.eof = False

    def fileno(self) -> int:
        return self.sock.fileno()

    def send_line(self, line: str) -> None:
        self.sock.sendall((line + "\n").encode("ascii"))

    def _recv_available(self) -> None:
        """Read whatever is already queued on the socket (non-blocking)."""
        self.sock.setblocking(False)
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    self.eof = True
                    return
                self.buf += chunk
        except BlockingIOError:
            return
        finally:
            self.sock.setblocking(True)

    def pop_line(self) -> Optional[str]:
        nl = self.buf.find(b"\n")
        if nl < 0:
            return None
        raw = self.buf[:nl]
        self.buf = self.buf[nl + 1 :]
        if raw.endswith(b"\r"):
            raw = raw[:-1]
        return raw.decode("ascii")

    def drain_lines(self) -> List[str]:
        self._recv_available()
        lines: List[str] = []
        while True:
            line = self.pop_line()
            if line is None:
                break
            lines.append(line)
        return lines


def connect(host: str, port: int, attempts: int = 50) -> LineSocket:
    last: Optional[BaseException] = None
    for _ in range(attempts):
        try:
            sock = socket.create_connection((host, port), timeout=2)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return LineSocket(sock)
        except OSError as exc:
            last = exc
            time.sleep(0.05)
    raise SystemExit(f"could not connect to {host}:{port}: {last}")


def load_toml(path: Path) -> Dict[str, Any]:
    with path.open("rb") as fh:
        return tomllib.load(fh)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="scripts/strategy.toml", help="Strategy TOML (CLI overrides file)")
    ap.add_argument("--host", default=None)
    ap.add_argument("--feed-port", type=int, default=None)
    ap.add_argument("--order-port", type=int, default=None)
    ap.add_argument("--lookahead-ns", type=int, default=None, help="Declared order latency Δ")
    ap.add_argument("--interval-ns", type=int, default=None, help="Strategy period")
    ap.add_argument("--class", dest="class_code", default=None)
    ap.add_argument("--sec", default=None)
    ap.add_argument("--qty", type=int, default=None)
    ap.add_argument("--tif", type=int, default=None, choices=[0, 1, 2], help="0=DAY 1=IOC 2=FOK")
    ap.add_argument("--max-orders", type=int, default=None)
    ap.add_argument("--verbose", action="store_true", help="Log every TIME")
    args = ap.parse_args()

    cfg_path = Path(args.config)
    file_cfg: Dict[str, Any] = {}
    if cfg_path.is_file():
        file_cfg = load_toml(cfg_path)
        print(f"# config {cfg_path}", file=sys.stderr)
    else:
        print(f"# warning: no {cfg_path}, using CLI/defaults", file=sys.stderr)

    host = args.host if args.host is not None else file_cfg.get("host", "127.0.0.1")
    feed_port = args.feed_port if args.feed_port is not None else int(file_cfg.get("feed_port", 9001))
    order_port = args.order_port if args.order_port is not None else int(file_cfg.get("order_port", 9002))
    lookahead_ns = (
        args.lookahead_ns if args.lookahead_ns is not None else int(file_cfg.get("lookahead_ns", 100_000_000))
    )
    interval_ns = (
        args.interval_ns if args.interval_ns is not None else int(file_cfg.get("interval_ns", 300_000_000_000))
    )
    class_code = args.class_code if args.class_code is not None else file_cfg.get("class_code", "FCEU")
    sec_code = args.sec if args.sec is not None else file_cfg.get("sec_code", "FCEU SI 20260615 PS")
    qty = args.qty if args.qty is not None else int(file_cfg.get("qty", 1))
    tif = args.tif if args.tif is not None else int(file_cfg.get("tif", 0))
    max_orders = args.max_orders if args.max_orders is not None else int(file_cfg.get("max_orders", 20))

    if lookahead_ns <= 1:
        raise SystemExit("lookahead must be > 1")
    if tif not in (0, 1, 2):
        raise SystemExit("tif must be 0 (DAY), 1 (IOC), or 2 (FOK)")
    tif_name = {0: "DAY", 1: "IOC", 2: "FOK"}[tif]
    print(f"# strategy qty={qty} tif={tif}({tif_name}) max_orders={max_orders}", file=sys.stderr)

    print(f"# connecting order {host}:{order_port} ...", file=sys.stderr)
    orders = connect(host, order_port)
    print(f"# connecting feed  {host}:{feed_port} ...", file=sys.stderr)
    feed = connect(host, feed_port)

    orders.send_line(f"HELLO|{lookahead_ns}")
    print(f"# HELLO lookahead={lookahead_ns}", file=sys.stderr)

    feed_h = 0
    order_h = 0
    engine_time = 0
    ready = False
    feed_done = False
    order_done = False
    book = LocalBook()
    top = BookTop()
    next_fire: Optional[int] = None
    client_id = 1000
    orders_sent = 0
    fills = 0
    accepts = 0
    rejects = 0
    cancels = 0
    bye_sent = False
    wall0 = time.perf_counter()

    def set_clock(t: int, why: str) -> None:
        nonlocal engine_time
        if t < engine_time:
            return
        engine_time = t
        orders.send_line(f"TIME|{engine_time}")
        if args.verbose:
            print(f"# TIME|{engine_time} ({why})", file=sys.stderr)

    def maybe_trade() -> None:
        nonlocal client_id, orders_sent, next_fire
        if not ready or top.bid_px is None or next_fire is None:
            return
        if orders_sent >= max_orders:
            return
        if engine_time < next_fire:
            return

        px = top.bid_px
        cl = client_id
        client_id += 1
        orders_sent += 1
        orders.send_line(f"N|{cl}|{engine_time}|{class_code}|{sec_code}|S|{px}|{qty}|{tif}")
        print(
            f"# STRATEGY sell@{px} qty={qty} tif={tif}({tif_name}) ts_seen={engine_time} cl_id={cl}",
            file=sys.stderr,
        )
        next_fire = engine_time + interval_ns

    def handle_order_line(line: str) -> None:
        nonlocal order_h, ready, order_done, fills, accepts, rejects, cancels, next_fire
        tag = line.split("|", 1)[0]
        if tag == "K":
            ready = True
            start_ts = int(line.split("|")[1])
            next_fire = start_ts + interval_ns
            print(f"# K start_ts={start_ts}, first fire at {next_fire}", file=sys.stderr)
            set_clock(start_ts, "after K")
        elif tag == "H":
            order_h = int(line.split("|")[1])
        elif tag == "E":
            order_done = True
            print("# order channel E", file=sys.stderr)
        elif tag == "A":
            accepts += 1
            print(f"# A {line}", file=sys.stderr)
        elif tag == "R":
            rejects += 1
            print(f"# R REJECT {line}", file=sys.stderr)
        elif tag == "F":
            fills += 1
            print(f"# F FILL {line}", file=sys.stderr)
        elif tag == "X":
            cancels += 1
            print(f"# X CANCEL {line}", file=sys.stderr)
        elif tag == "Y":
            print(f"# Y {line}", file=sys.stderr)

    def handle_feed_line(line: str) -> None:
        nonlocal feed_h, feed_done, top
        fields = line.split("|")
        tag = fields[0]
        if tag == "S":
            # S|seq|ts|class|sec|nbid|nask|px:qty|...
            snap_class, snap_sec = fields[3], fields[4]
            if snap_class == class_code and snap_sec == sec_code:
                nbid, nask = int(fields[5]), int(fields[6])
                top = book.apply_snapshot(int(fields[2]), fields[7:], nbid, nask)
        elif tag == "I":
            # I|seq|ts|class|sec|n|B:px:qty|...
            snap_class, snap_sec = fields[3], fields[4]
            if snap_class == class_code and snap_sec == sec_code:
                n = int(fields[5])
                top = book.apply_increment(int(fields[2]), fields[6 : 6 + n])
        elif tag == "H":
            feed_h = int(fields[1])
        elif tag == "E":
            feed_done = True
            print("# feed channel E", file=sys.stderr)

    def chase_horizons() -> None:
        if not ready or feed_h == 0 or order_h == 0:
            return
        safe = min(feed_h, order_h)
        if safe > engine_time:
            set_clock(safe, "horizons")
            maybe_trade()

    def pump() -> bool:
        """Drain both sockets; return True if any line was handled."""
        progressed = False
        for line in orders.drain_lines():
            handle_order_line(line)
            progressed = True
        for line in feed.drain_lines():
            handle_feed_line(line)
            progressed = True
        return progressed

    while not (feed_done and order_done):
        while pump():
            chase_horizons()
            maybe_trade()

        chase_horizons()
        maybe_trade()

        if not bye_sent and orders_sent >= max_orders and (accepts + rejects) >= orders_sent:
            orders.send_line("BYE")
            bye_sent = True
            print("# BYE", file=sys.stderr)

        if feed_done and order_done:
            break

        if orders.eof and feed.eof:
            print("# both sockets closed", file=sys.stderr)
            break

        # Block until either channel has data — no fixed 10–20 ms sleeps.
        readable, _, _ = select.select([orders, feed], [], [], 5.0)
        if not readable:
            print("# timeout waiting for venue", file=sys.stderr)
            if not bye_sent:
                orders.send_line("BYE")
            break

    wall = time.perf_counter() - wall0
    print(
        f"# done: orders={orders_sent} accepts={accepts} rejects={rejects} "
        f"cancels={cancels} fills={fills} engine_time={engine_time} wall={wall:.3f}s",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
