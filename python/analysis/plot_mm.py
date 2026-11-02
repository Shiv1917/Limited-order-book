"""Market-making backtest: equity curves for several latencies + markout histogram.

usage: PYTHONPATH=build python plot_mm.py data/aapl.obm [--from 09:30:00] [--out mm.png]
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
    ap.add_argument("--from", dest="quote_from", default="09:30:00")
    ap.add_argument("--latencies", default="0,10,100")
    ap.add_argument("--out", default="mm.png")
    a = ap.parse_args()

    lats = [int(x) for x in a.latencies.split(",")]
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.2))
    for i, lat in enumerate(lats):
        r = pyob.backtest(a.stream, latency_msgs=lat, quote_from=a.quote_from, improve=False, skew=2.0,
                          equity_every=200)
        print(f"--- latency {lat} msgs ---\n{r['summary']}")
        t = r["equity_t"] / 3.6e12
        ax1.plot(t, r["equity"], color=style.SERIES[i], label=f"latency {lat} msgs (P&L ${r['pnl']:,.0f})")
        if i == 0:
            h, m, sec = (int(x) for x in a.quote_from.split(":"))
            ax1.set_xlim(h + m / 60 + sec / 3600, t[-1])
            # Spread captured at the moment of each fill (before any adverse move).
            edge = r["fill_side"] * (r["fill_mid"] - r["fill_price"]) / 1e4 * 100  # cents captured vs mid
            ax2.hist(edge, bins=np.arange(-5, 5.25, 0.25), color=style.SERIES[0])
            ax2.set_title("Edge at fill vs mid (latency 0)")
            ax2.set_xlabel("cents per share (positive = bought below / sold above mid)")
            ax2.set_ylabel("fills")
    ax1.axhline(0, color=style.TEXT_2, linewidth=0.8)
    ax1.set_title("Mark-to-mid P&L")
    ax1.set_xlabel("hours after midnight (exchange time)")
    ax1.set_ylabel("$")
    ax1.legend(loc="lower left", fontsize=8)
    fig.tight_layout()
    fig.savefig(a.out, dpi=150)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
