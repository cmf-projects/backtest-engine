#!/usr/bin/env python3
"""Generate correlated white-noise book tapes: spot / futures / option JSONL.

Each line:
  {"seq":N,"ts_ns":...,"symbol":"...","bids":[{"price":..,"qty":..}],"asks":[...]}

Spot mid ~ mean + noise; fut = spot + basis + noise; option ~ max(spot-K, tick) + noise.
"""

from __future__ import annotations

import argparse
import random
from pathlib import Path


def round_tick(px: float, tick: float) -> float:
    return round(px / tick) * tick


def levels(mid: float, tick: float, spread_ticks: int, qty: int) -> tuple[list[dict], list[dict]]:
    half = spread_ticks * tick
    bid = round_tick(mid - half, tick)
    ask = round_tick(mid + half, tick)
    # Venue rejects price <= 0 (OnGrid); keep a positive one-tick book.
    if bid < tick:
        bid = tick
    if ask <= bid:
        ask = bid + tick
    return [{"price": bid, "qty": qty}], [{"price": ask, "qty": qty}]


def write_line(f, seq: int, ts_ns: int, symbol: str, bids: list, asks: list) -> None:
    # Hand-rolled JSON for speed (1M lines).
    f.write(
        f'{{"seq":{seq},"ts_ns":{ts_ns},"symbol":"{symbol}",'
        f'"bids":[{{"price":{bids[0]["price"]:.4f},"qty":{bids[0]["qty"]}}}],'
        f'"asks":[{{"price":{asks[0]["price"]:.4f},"qty":{asks[0]["qty"]}}}]}}\n'
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--n", type=int, default=1_000_000, help="events per tape")
    ap.add_argument("--out-dir", type=Path, default=Path("testdata/population/noise"))
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mean", type=float, default=100.0)
    ap.add_argument("--sigma", type=float, default=0.05, help="spot innovation std")
    ap.add_argument("--tick", type=float, default=0.01)
    ap.add_argument("--step-ns", type=int, default=1_000_000, help="ts step between events")
    ap.add_argument("--start-ns", type=int, default=1_000_000_000)
    ap.add_argument("--basis", type=float, default=0.50, help="fut - spot mean")
    ap.add_argument("--strike", type=float, default=100.0, help="option strike (call-ish)")
    args = ap.parse_args()

    if args.n < 1:
        raise SystemExit("--n must be >= 1")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    spot_path = args.out_dir / "spot.jsonl"
    fut_path = args.out_dir / "fut.jsonl"
    opt_path = args.out_dir / "option.jsonl"

    rng = random.Random(args.seed)
    spot_mid = args.mean

    print(
        f"gen_noise_tapes: n={args.n} out={args.out_dir} mean={args.mean} "
        f"sigma={args.sigma} seed={args.seed}",
        flush=True,
    )

    with spot_path.open("w", encoding="utf-8") as fs, fut_path.open(
        "w", encoding="utf-8"
    ) as ff, opt_path.open("w", encoding="utf-8") as fo:
        for i in range(args.n):
            seq = i + 1
            ts = args.start_ns + i * args.step_ns

            spot_mid += rng.gauss(0.0, args.sigma)
            # Soft mean reversion so it does not wander off forever.
            spot_mid += 0.001 * (args.mean - spot_mid)

            fut_mid = spot_mid + args.basis + rng.gauss(0.0, args.sigma * 0.5)
            opt_mid = max(spot_mid - args.strike, args.tick) + abs(rng.gauss(0.0, args.sigma))

            sb, sa = levels(spot_mid, args.tick, 1, 100)
            fb, fa = levels(fut_mid, args.tick, 1, 100)
            ob, oa = levels(opt_mid, args.tick, 1, 50)

            write_line(fs, seq, ts, "SBER", sb, sa)
            write_line(ff, seq, ts, "SBER_FUT", fb, fa)
            write_line(fo, seq, ts, "SBER_OPT", ob, oa)

            if seq % 100_000 == 0:
                print(f"  … {seq}/{args.n}", flush=True)

    print(f"wrote {spot_path}\n      {fut_path}\n      {opt_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
