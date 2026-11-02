"""Plot latency percentile curves written by `ob_bench --out-dir DIR`.

usage: python plot_latency.py RESULTS_DIR [--out latency.png]
"""
import argparse
import csv
import glob
import os

import style

style.apply()
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FixedLocator, FixedFormatter  # noqa: E402


def read_hgrm(path):
    name, pts = None, []
    with open(path) as f:
        for line in f:
            if line.startswith("# "):
                name = line[2:].strip()
                continue
            parts = line.split()
            if len(parts) >= 3 and not line.startswith("#"):
                try:
                    v, p = float(parts[0]), float(parts[1])
                except ValueError:
                    continue
                if p < 1.0:
                    pts.append((1.0 / (1.0 - p), v))
    return name, pts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_dir")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    order = []
    summary = os.path.join(a.results_dir, "summary.csv")
    if os.path.exists(summary):
        with open(summary) as f:
            order = [r["version"] for r in csv.DictReader(f)]
    curves = dict(read_hgrm(p) for p in glob.glob(os.path.join(a.results_dir, "*.hgrm")))
    names = [n for n in order if n in curves] or sorted(curves)

    fig, ax = plt.subplots(figsize=(8, 4.5))
    for i, n in enumerate(names):
        xs, ys = zip(*curves[n])
        ax.plot(xs, ys, color=style.SERIES[i], label=n)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ticks = [2, 10, 100, 1000, 10000]
    ax.xaxis.set_major_locator(FixedLocator(ticks))
    ax.xaxis.set_major_formatter(FixedFormatter(["p50", "p90", "p99", "p99.9", "p99.99"]))
    ax.xaxis.set_minor_locator(FixedLocator([]))
    ax.set_xlabel("percentile")
    ax.set_ylabel("latency per message (ns, log)")
    ax.set_title("Per-message latency by optimisation stage")
    ax.legend(loc="upper left", fontsize=8)
    fig.tight_layout()
    out = a.out or os.path.join(a.results_dir, "latency.png")
    fig.savefig(out, dpi=150)
    print("wrote", out)


if __name__ == "__main__":
    main()
