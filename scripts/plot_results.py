#!/usr/bin/env python3
"""Plot DaCapo / JFR benchmark results from a run directory.

Usage:
    scripts/plot_results.py <run-dir>

Produces three PNGs in <run-dir>/:
    - throughput.png        ms-per-iteration time series, per benchmark
    - pause_cdf.png         CDF of GC pause times (p50, p99, p99.9)
    - barrier_cycles.png    per-load barrier cycle estimate from PMU samples

The script handles the case where JFR isn't available yet (Phase 0) by
falling back to parsed DaCapo stdout.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

try:
    import matplotlib.pyplot as plt  # type: ignore
except ImportError:
    print("matplotlib not installed; run: pip install matplotlib", file=sys.stderr)
    sys.exit(1)


DACAPO_ITER_RE = re.compile(
    r"=====\s+DaCapo[^\s]*\s+(\S+)\s+PASSED in (\d+) msec\s+====="
)


def parse_dacapo_log(log_path: Path) -> list[int]:
    """Return list of per-iteration milliseconds from a DaCapo run log."""
    times = []
    for line in log_path.read_text().splitlines():
        m = DACAPO_ITER_RE.search(line)
        if m:
            times.append(int(m.group(2)))
    return times


def plot_throughput(run_dir: Path) -> None:
    fig, ax = plt.subplots(figsize=(8, 4))
    for log_path in sorted(run_dir.glob("*.log")):
        bench = log_path.stem
        times = parse_dacapo_log(log_path)
        if not times:
            continue
        ax.plot(range(1, len(times) + 1), times, marker="o", label=bench)
    ax.set_xlabel("Iteration")
    ax.set_ylabel("Time (ms)")
    ax.set_title("DaCapo iteration time")
    ax.legend()
    ax.grid(True, alpha=0.3)
    out = run_dir / "throughput.png"
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")


def plot_pause_cdf(run_dir: Path) -> None:
    """Parse JFR pause events if available; otherwise emit a stub."""
    jfrs = list(run_dir.glob("*.jfr"))
    if not jfrs:
        print("no JFR files; skipping pause_cdf (Phase 0 expected)")
        return
    # TODO(phase1): real JFR parsing via jfr2csv or jfrparser. For now,
    # accept a sibling .pauses.json that the FVP harness can dump.
    fig, ax = plt.subplots(figsize=(8, 4))
    plotted = False
    for jfr in jfrs:
        pauses_json = jfr.with_suffix(".pauses.json")
        if not pauses_json.exists():
            continue
        pauses = sorted(json.loads(pauses_json.read_text()))
        if not pauses:
            continue
        ys = [(i + 1) / len(pauses) for i in range(len(pauses))]
        ax.plot(pauses, ys, label=jfr.stem)
        plotted = True
    if not plotted:
        print("no .pauses.json siblings; skipping")
        return
    ax.set_xscale("log")
    ax.set_xlabel("Pause time (µs)")
    ax.set_ylabel("CDF")
    ax.set_title("GC pause CDF")
    ax.legend()
    ax.grid(True, alpha=0.3)
    out = run_dir / "pause_cdf.png"
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")


def plot_barrier_cycles(run_dir: Path) -> None:
    """Plot per-load barrier cycles from PMU sample JSONs.

    Expected format: <run-dir>/<bench>.pmu.json with shape
        { "barrier_cycles_per_load": [...samples...] }
    """
    fig, ax = plt.subplots(figsize=(8, 4))
    plotted = False
    for pmu in sorted(run_dir.glob("*.pmu.json")):
        data = json.loads(pmu.read_text())
        samples = data.get("barrier_cycles_per_load") or []
        if not samples:
            continue
        ax.hist(samples, bins=40, alpha=0.6, label=pmu.stem)
        plotted = True
    if not plotted:
        print("no PMU samples; skipping (Phase 2 will populate)")
        return
    ax.set_xlabel("Cycles per reference load (barrier slot)")
    ax.set_ylabel("Frequency")
    ax.set_title("ZGC load barrier cost")
    ax.legend()
    ax.grid(True, alpha=0.3)
    out = run_dir / "barrier_cycles.png"
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    args = parser.parse_args()

    if not args.run_dir.is_dir():
        print(f"not a directory: {args.run_dir}", file=sys.stderr)
        return 2

    plot_throughput(args.run_dir)
    plot_pause_cdf(args.run_dir)
    plot_barrier_cycles(args.run_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
