# ob-sim: validation

A backtester can run cleanly and still be wrong, so correctness is argued in
six layers, cheapest first.

* Layers 1–4 run in `ctest` and in CI on every commit.
* Layers 5 and 6 use the AAPL day, which is not in the repository; rebuild it
  with `ob_itch2lobster` (see [design](sim_design.md#data-itch--lobster)).
* All numbers below are from 2026-11-02 on macOS (Apple clang 17). The suite
  also passes with GCC 15.

## 1. Hand-built scenarios

There are 21 files in `sim/tests/scenarios/`. Each scenario sets up a tiny
message file and a list of scripted orders, with the expected exchange events
written by hand. They cover:

* the plan's worked example (fills 15 then 5, rebate, cash);
* executions of the order ahead (no fill) versus behind (fill);
* a cancel ahead improving queue position;
* trade-through: on, off, and capped at trade size;
* `exec_volume_share`;
* cancel success and cancel too late;
* IOC remainder, multi-level take then rest, market order;
* `own_taken`: no double count, and release by history;
* self-trade, order size, position and open-order rejects;
* invalid price or qty, and cancelling a rejected order;
* a halt pulling orders and blocking new ones until resume;
* hidden executions never filling;
* latency costing queue position;
* market data winning timestamp ties.

**Result: all 21 pass.** Writing them changed one rule: the session ends at
the last message, so scenarios with actions after the last real event end
with a harmless closing message.

## 2. Accounting

These tests feed hand-written fill lists to the portfolio: a round trip with
fees and rebates, a partial close at average cost, a flip through zero, and a
5,000-fill random sequence.

Each checks `realized + unrealized − fees == equity − equity_0` exactly
(integer arithmetic). **All pass.**

## 3. Book parity (no-impact check)

**AAPL 2019-01-30**, 04:00–09:58:57, 328,382 rows, compared after every
message:

| Run | Checkpoints | Depth(10) mismatches |
|---|---|---|
| `ob-sim check` (null strategy) | 328,382 | 0 |

**Generated streams.** A generator in `test_properties.cpp` builds valid
streams. It computes its snapshots with an independent aggregate model, not
with `ob::Book`. On these streams:

* parity holds after every message while a market maker trades heavily;
* the final book, the arrival sequence counter and the full mid-price
  history are identical to a do-nothing run.

**LOBSTER fixture** (`tests/data`, which includes seeded liquidity): parity
holds while a market maker trades.

## 4. Invariants and determinism

These are checked on every event, in every run, and reported as
`invariant_violations` in `summary.json`:

* the clock never decreases;
* no fill comes before its order's arrival;
* filled ≤ order quantity, and leaves are consistent with fills;
* portfolio position == exchange position.

A fuzz test runs 12 seeds × 4,000 generated messages × 400 random scripted
actions: limit, IOC and market orders, cancels, off-tick prices, and
halt/hidden messages, with jittered latency. It also checks that:

* position equals the signed sum of fills;
* max |position| ≤ the risk limit;
* the accounting identity holds.

**Result: 0 violations.**

**Determinism:**

* The same config gives byte-identical `orders.csv`, `fills.csv` and
  `pnl.csv` (`scripts/sim_determinism.sh`).
* A different seed with jitter gives a different hash.
* The full AAPL market-maker run hashes to the same value (`d45dbe09…`) with
  Apple clang 17 and with GCC 15.

**Sanitizers:** the full simulator suite and a full-day AAPL market-maker
run are clean under UBSan locally, and the run's output hash is unchanged.

ASan was not verified locally: Apple's ASan runtime hangs at startup on this
macOS, as noted in the README, for the book's existing tests too. The CI
`sanitizers` job runs the whole suite and an `ob-sim` run under ASan + UBSan
on Linux.

**Not done from the plan:** the plan's stale-BookView build, a BookView
deliberately returning older state, was not built. The latency stage already
makes the strategy act on a book that is stale by `feed + proc +
to_exchange`, and the sweep in layer 6 is the directional check.

## 5. Independent Python reference

`ref_py/fill_model.py` (245 lines) was written by a separate agent from a
written spec of the rules only. It was not allowed to read any C++ source; it
ran `ob-sim` only as a black box. Its model is a dict of price levels, a list
per level, and an `OWN` marker in the list instead of sequence numbers.

The comparison is on the probe strategy: buy 100 at the best bid every 10 s,
cancel after 5 s, 50 µs entry latency, 09:30–09:58.

| Run | C++ fills | Python fills | Matched |
|---|---|---|---|
| Default probe (170 orders) | 212 | 212 | **212 / 212 (identical)** |
| Overlapping orders (P = 3 s, H = 20 s, from before the open) | 826 | 826 | identical |
| Orders resting through the 09:30 opening cross | 152 | 152 | identical |

The Python book also matched all 328,382 snapshots on its own.

**Gap:** on this data the probe never takes liquidity, so the ask walk,
`own_taken` and halt rejects are not cross-checked on real data. Those paths
are covered by layer-1 scenarios 10–14 and 18, and by the fuzz test.

To reproduce (about 1 minute):

```
./build/sim/ob-sim run --config sim/configs/aapl_probe.json
python3 ref_py/fill_model.py --messages data/AAPL_2019-01-30_message_10.csv \
    --orderbook data/AAPL_2019-01-30_orderbook_10.csv --out runs/aapl_probe/fills_py.csv
python3 ref_py/compare.py --cpp runs/aapl_probe/fills.csv --py runs/aapl_probe/fills_py.csv
```

## 6. Behavioural sanity

### Latency sweep

This is the market maker (`sim/configs/aapl_mm.json`) over
`latency_ns.to_exchange`. The other delays stay at 20/10/50 µs plus jitter.

| to_exchange | Net PnL | Fills | Fill rate | Queue wait (s) | Adverse sel. 1 s (ticks) | Taker shares |
|---|---|---|---|---|---|---|
| 0 | −$582 | 2,123 | 0.0543 | 0.292 | 1.52 | 8,529 |
| 10 µs | −$526 | 2,051 | 0.0523 | 0.303 | 1.52 | 8,895 |
| 50 µs | −$699 | 2,064 | 0.0524 | 0.303 | 1.55 | 10,916 |
| 200 µs | −$784 | 2,162 | 0.0545 | 0.298 | 1.55 | 13,138 |
| 1 ms | −$1,179 | 2,608 | 0.0595 | 0.306 | 1.47 | 33,506 |
| 5 ms | −$1,755 | 3,017 | 0.0626 | 0.331 | 1.50 | 52,476 |

**The fill rate rises with latency.** The plan says a rising fill rate means
"suspect a bug or lookahead", so it was investigated before being reported.
The cause is trade-through fills of stale quotes:

* A bid placed at the touch that arrives 5 ms later, after the market has
  dropped, sits above the new best bid.
* The next sell execution then fills it, by the trade-through rule.
* Maker shares filled at or through the mid rise from 21k (0 latency) to
  54k (5 ms).

Run with trade-through switched off, the same sweep behaves as the plan
expects: the fill rate falls steadily and queue waits lengthen.

| to_exchange | 0 | 10 µs | 50 µs | 200 µs | 1 ms | 5 ms |
|---|---|---|---|---|---|---|
| Fill rate | 0.0255 | 0.0244 | 0.0242 | 0.0238 | 0.0230 | 0.0211 |
| Queue wait (s) | 0.329 | 0.337 | 0.363 | 0.360 | 0.443 | 0.462 |

Net PnL falls with latency in both settings. Taker shares grow as well,
because more quotes are already marketable when they arrive.

### Other checks

* **Fees off vs on** (test): fills are identical, and net PnL differs by
  exactly the fees paid.
* **Market-maker result at 50 µs:**
  * spread capture is +$1,240 and inventory PnL −$2,173, for −$932 gross;
    the rebate (−$233 fees) improves that to −$699 net;
  * maker adverse selection is about 1.55 ticks at 1 s;
  * this is the usual result for naive passive quoting: it earns the spread
    and then gets picked off. The day after Apple's earnings was a falling,
    volatile morning.
* **Imbalance taker** (`sim/configs/aapl_imbalance.json`):
  * net −$210: inventory PnL +$8 against −$184 spread-crossing cost and $34
    of fees;
  * the signal had at most a weak edge before costs.

## Performance

Simulator throughput on the AAPL day (Apple M-series laptop, no core
isolation, so numbers are noisy):

| Run | Events | Events/s |
|---|---|---|
| Null strategy (replay + exchange layer only) | 328k | ~21–24 M |
| Market maker (+113k simulator events) | 441k | ~3–13 M across repeat runs |

The market maker's per-event cost comes from the strategy callback, the
`std::map`-based order bookkeeping in the exchange layer, and the in-memory
logs. It was not profiled with `perf`, which is unavailable on macOS;
`scripts/bench_linux.sh` is the place to add that.
