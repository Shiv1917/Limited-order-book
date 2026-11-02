"""Replay an .obm stream through the engine and plot top of book and spread.

usage: PYTHONPATH=build python plot_book.py data/aapl.obm [--out book.png]
"""
import argparse

import numpy as np
import style

style.apply()
import matplotlib.pyplot as plt  # noqa: E402
import pyob  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stream")
    ap.add_argument("--every", type=int, default=50)
    ap.add_argument("--out", default="book.png")
    a = ap.parse_args()

    meta = pyob.load_stream(a.stream)
    d = pyob.replay_top_of_book(a.stream, every=a.every)
    hours = d["t"] / 3.6e12
    bid, ask = d["bid"] / 1e4, d["ask"] / 1e4
    spread_c = (d["ask"] - d["bid"]) / 100.0  # cents

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 6), sharex=True, gridspec_kw={"height_ratios": [2, 1]})
    ax1.plot(hours, bid, color=style.SERIES[0], linewidth=1.2, label="best bid")
    ax1.plot(hours, ask, color=style.SERIES[1], linewidth=1.2, label="best ask")
    lo, hi = np.nanpercentile(bid, 0.5), np.nanpercentile(ask, 99.5)
    ax1.set_ylim(lo - 0.5, hi + 0.5)  # ignore stray pre-market quotes far from the market
    ax1.set_ylabel("price ($)")
    ax1.set_title(f"{meta['symbol']}: top of book reconstructed by the engine")
    ax1.legend(loc="upper left")
    ax2.plot(hours, np.clip(spread_c, 0, None), color=style.SERIES[0], linewidth=1.0)
    ax2.set_yscale("symlog", linthresh=1)
    ax2.set_ylabel("spread (cents)")
    ax2.set_xlabel("hours after midnight (exchange time)")
    fig.tight_layout()
    fig.savefig(a.out, dpi=150)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
