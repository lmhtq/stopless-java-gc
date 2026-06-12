#!/usr/bin/env python3
"""Generate the StoplessGC paper figures from paper/data/*.txt.

Palette: Okabe-Ito (colorblind-safe, standard in systems papers).
Output: PDF vector figures sized for an acmsmall text block (~5.5in).
"""
import re
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
FIGS = os.path.join(HERE, "figs")
os.makedirs(FIGS, exist_ok=True)

# Okabe-Ito
BLUE      = "#0072B2"
VERMILION = "#D55E00"
GREEN     = "#009E73"
SKY       = "#56B4E9"
GRAY      = "#666666"

plt.rcParams.update({
    "font.family": "serif",
    "font.size": 9,
    "axes.titlesize": 9,
    "axes.labelsize": 9,
    "legend.fontsize": 8,
    "xtick.labelsize": 8,
    "ytick.labelsize": 8,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "grid.color": "#DDDDDD",
    "grid.linewidth": 0.6,
    "axes.axisbelow": True,
    "figure.dpi": 200,
    "savefig.bbox": "tight",
    "pdf.fonttype": 42,
})

LINE = re.compile(
    r"roots=(\d+).*?moved=(\d+).*?heap_used_K=(\d+)\s+"
    r"scan_move_us=([\d.]+)\s+revoke_us=([\d.]+)\s+pause_us=([\d.]+)")
LINE_B = re.compile(
    r"roots=(\d+).*?swept=(\d+)\s+batch=(\d+)\s+heap_used_K=(\d+)\s+"
    r"scan_move_us=([\d.]+)\s+revoke_us=([\d.]+)\s+pause_us=([\d.]+)")


def parse_heap(path):
    rows = []
    with open(path) as f:
        for ln in f:
            m = LINE.search(ln)
            if m:
                rows.append(dict(
                    roots=int(m.group(1)), heap_mb=int(m.group(3)) / 1024.0,
                    scan_ms=float(m.group(4)) / 1e3,
                    revoke_s=float(m.group(5)) / 1e6,
                    pause_s=float(m.group(6)) / 1e6))
    return rows


def parse_batched(path):
    runs = {}
    cur = None
    with open(path) as f:
        for ln in f:
            if ln.startswith("=== batch="):
                cur = int(ln.split("batch=")[1].split() [0].strip("= "))
                runs[cur] = []
                continue
            m = LINE_B.search(ln)
            if m and cur is not None:
                runs[cur].append(dict(
                    swept=int(m.group(2)),
                    scan_ms=float(m.group(5)) / 1e3,
                    revoke_s=float(m.group(6)) / 1e6,
                    pause_s=float(m.group(7)) / 1e6))
    return runs


# ---------------------------------------------------------------- figure 1
def fig_heap_independence():
    rows = parse_heap(os.path.join(DATA, "c11_heap_independence_stoplessbench.txt"))
    # post-warmup segment: the root set has stabilised (constant 894 roots),
    # isolating heap size as the only variable.
    steady = [r for r in rows if r["roots"] == 894]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(5.4, 2.1))

    x = [r["heap_mb"] for r in steady]
    ax1.plot(x, [r["scan_ms"] for r in steady], "o-", color=BLUE,
             ms=2.6, lw=1.1)
    ax1.set_xlabel("heap used (MiB)")
    ax1.set_ylabel("relocation pause (ms)")
    ax1.set_ylim(0, 30)
    ax1.set_title("(a) scan + relocate + heal: flat", loc="left")
    ax1.annotate("+7% over a 46$\\times$ heap",
                 xy=(x[-1], steady[-1]["scan_ms"]), xytext=(58, 22),
                 fontsize=8, color=GRAY,
                 arrowprops=dict(arrowstyle="-", color=GRAY, lw=0.7))

    ax2.plot(x, [r["revoke_s"] for r in steady], "s-", color=VERMILION,
             ms=2.6, lw=1.1)
    ax2.set_xlabel("heap used (MiB)")
    ax2.set_ylabel("revocation sweep (s)")
    ax2.set_ylim(0, 7)
    ax2.set_title("(b) whole-AS revocation: linear", loc="left")

    fig.tight_layout(w_pad=2.0)
    fig.savefig(os.path.join(FIGS, "heap_independence.pdf"))
    plt.close(fig)
    lo, hi = steady[0], steady[-1]
    print(f"fig1: n={len(steady)} heap {lo['heap_mb']:.1f}->{hi['heap_mb']:.1f} MiB "
          f"scan {lo['scan_ms']:.1f}->{hi['scan_ms']:.1f} ms "
          f"revoke {lo['revoke_s']:.2f}->{hi['revoke_s']:.2f} s")


# ---------------------------------------------------------------- figure 2
def fig_batched():
    runs = parse_batched(os.path.join(DATA, "c11_batched_revoke.txt"))

    fig, (ax1, ax2) = plt.subplots(
        1, 2, figsize=(5.4, 2.1), gridspec_kw={"width_ratios": [1.6, 1]})

    colors = {1: VERMILION, 8: SKY, 32: BLUE}
    for b in (1, 8, 32):
        ys = [r["pause_s"] for r in runs[b]]
        ax1.plot(range(len(ys)), ys, lw=0.9, color=colors[b],
                 label=f"batch = {b}", alpha=0.95)
    ax1.set_yscale("log")
    ax1.set_xlabel("collection cycle")
    ax1.set_ylabel("cycle pause (s)")
    ax1.set_title("(a) per-cycle pause", loc="left")
    ax1.legend(frameon=False, loc="center right")

    batches = [1, 8, 32]
    avgs = [sum(r["pause_s"] for r in runs[b]) / len(runs[b]) for b in batches]
    bars = ax2.bar([str(b) for b in batches], avgs,
                   color=[colors[b] for b in batches], width=0.62)
    for rect, v in zip(bars, avgs):
        ax2.annotate(f"{v:.2f} s", xy=(rect.get_x() + rect.get_width() / 2, v),
                     xytext=(0, 2), textcoords="offset points",
                     ha="center", fontsize=8, color="#333333")
    ax2.set_xlabel("revocation batch size")
    ax2.set_ylabel("mean pause (s)")
    ax2.set_ylim(0, max(avgs) * 1.22)
    ax2.set_title("(b) amortisation", loc="left")

    fig.tight_layout(w_pad=2.0)
    fig.savefig(os.path.join(FIGS, "batched_revoke.pdf"))
    plt.close(fig)
    for b, a in zip(batches, avgs):
        n = len(runs[b]); sw = sum(r["swept"] for r in runs[b])
        print(f"fig2: batch={b} cycles={n} sweeps={sw} mean_pause={a:.3f}s "
              f"({avgs[0]/a:.1f}x lower)")


if __name__ == "__main__":
    fig_heap_independence()
    fig_batched()
    print("figures written to", FIGS)
