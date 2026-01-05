#!/usr/bin/env python3

"""Summarize scripts/bench.sh CSV into a Markdown report.

Usage:
  python3 scripts/bench_summarize.py benchmarks/.../results.csv > report.md
"""

from __future__ import annotations

import csv
import os
import platform
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime


@dataclass
class Row:
    scenario: str
    mode: str
    threads: int
    symbols: int
    events: int
    sigma: float
    arena_bytes: int
    rep: int
    throughput: int
    steps_s: int
    book_ops_s: int
    action_ratio: float
    elapsed_ms: float
    wall_ms: float
    with_grpc: str


def _as_int(s: str) -> int:
    return int(float(s))


def _as_float(s: str) -> float:
    try:
        return float(s)
    except Exception:
        return float("nan")


def load_rows(path: str) -> list[Row]:
    rows: list[Row] = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append(
                Row(
                    scenario=r["scenario"],
                    mode=r["mode"],
                    threads=_as_int(r["threads"]),
                    symbols=_as_int(r["symbols"]),
                    events=_as_int(r["events"]),
                    sigma=_as_float(r["sigma"]),
                    arena_bytes=_as_int(r["arena_bytes"]),
                    rep=_as_int(r["rep"]),
                    throughput=_as_int(r["throughput_ev_s"]),
                    steps_s=_as_int(r["steps_per_s"]),
                    book_ops_s=_as_int(r["book_ops_per_s"]),
                    action_ratio=_as_float(r["action_ratio"]),
                    elapsed_ms=_as_float(r.get("elapsed_ms", "nan")),
                    wall_ms=_as_float(r.get("wall_ms", "nan")),
                    with_grpc=r.get("with_grpc", ""),
                )
            )
    return rows


def fmt_int(n: float | int) -> str:
    try:
        return f"{int(n):,}"
    except Exception:
        return "-"


def fmt_float(x: float, nd: int = 3) -> str:
    if x != x:  # NaN
        return "-"
    return f"{x:.{nd}f}"


def summarize(rows: list[Row]) -> str:
    by_scenario: dict[str, list[Row]] = defaultdict(list)
    for r in rows:
        by_scenario[r.scenario].append(r)

    lines: list[str] = []

    lines.append("# PMR Market Simulator Bench Report")
    lines.append("")
    lines.append(f"Generated: {datetime.now().isoformat(timespec='seconds')}")
    lines.append(f"Host: {platform.platform()}")
    lines.append("")

    lines.append("## Summary")
    lines.append("")
    lines.append(
        "| Scenario | Mode | Threads | Symbols | Events | Best Throughput (ev/s) | Avg Throughput (ev/s) | Best Book Ops/s | Avg Book Ops/s | Action Ratio (avg) |"
    )
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")

    for scenario in sorted(by_scenario.keys()):
        rs = sorted(by_scenario[scenario], key=lambda x: x.rep)

        mode = rs[0].mode
        threads = rs[0].threads
        symbols = rs[0].symbols
        events = rs[0].events

        tputs = [r.throughput for r in rs]
        opss = [r.book_ops_s for r in rs]
        ratios = [r.action_ratio for r in rs]

        best_t = max(tputs)
        avg_t = int(statistics.mean(tputs))
        best_o = max(opss)
        avg_o = int(statistics.mean(opss))
        avg_ratio = statistics.mean(ratios)

        lines.append(
            f"| {scenario} | {mode} | {threads} | {symbols} | {fmt_int(events)} | {fmt_int(best_t)} | {fmt_int(avg_t)} | {fmt_int(best_o)} | {fmt_int(avg_o)} | {fmt_float(avg_ratio, 3)} |"
        )

    lines.append("")
    lines.append("## Notes")
    lines.append("")
    lines.append("- **Throughput (ev/s)** is the simulator's printed `Throughput:` metric.")
    lines.append("- **Book Ops/s** is emitted ops (adds+cancels+trades). For multi-thread runs, this comes from the simulator's `Book ops/sec` line (max-thread time basis).")
    lines.append("- **Action ratio** = (adds+cancels+trades) / total steps.")
    lines.append("")

    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: bench_summarize.py <results.csv>", file=sys.stderr)
        return 2

    path = sys.argv[1]
    if not os.path.exists(path):
        print(f"File not found: {path}", file=sys.stderr)
        return 2

    rows = load_rows(path)
    if not rows:
        print("No rows found.")
        return 0

    print(summarize(rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
