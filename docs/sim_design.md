# ob-sim: design

`ob-sim` is a deterministic, single-instrument, event-driven exchange simulator.
It replays one ticker-day of LOBSTER messages through the order book in this
repository. A strategy trades against that replay, with queue-position fills
and a configurable latency model. The run writes fills, PnL and diagnostics to
files.

Related documents: [validation](sim_validation.md), [known biases](sim_biases.md).

## Layout

```
sim/
├── include/sim/
│   ├── time.hpp           Time/Price/Qty conventions, exact decimal formatting
│   ├── events.hpp         MdEvent, OrderRequest, OrderEvent, reasons
│   ├── event_queue.hpp    priority queue keyed (ts, priority, seq)
│   ├── latency.hpp        fixed delays + seeded jitter, FIFO clamp per stream
│   ├── book_view.hpp      read-only wrapper over ob::Book
│   ├── exchange.hpp       strategy orders, queue-aware fills, own_taken
│   ├── portfolio.hpp      cash, position, average-cost PnL, fees (integer)
│   ├── metrics.hpp        in-memory recorder for the output files
│   ├── simulator.hpp      SimCore + Simulator<Strategy>, the event loop
│   ├── run.hpp            config parsing, strategy selection, outputs
│   └── strategy/          null, scripted, probe, market_maker, imbalance
├── src/                   sim_core.cpp, run.cpp, sha256.cpp, main.cpp (ob-sim)
├── configs/               example configs for the AAPL day
└── tests/                 scenarios/*.scn, unit, property/fuzz tests
ref_py/                    independent Python fill model (validation layer 5)
tools/itch2lobster_main.cpp  ITCH -> LOBSTER converter (data source, see below)
```

## Changes to the existing book

These are the only changes to `include/ob/book.hpp` and `tools/lobster.*`, and
the book's own 117 tests still pass:

* **`seq`**: an arrival sequence number per resting order. It lives in what
  was padding in the packed 32-byte order, so the order is still 32 bytes.
  Reducing an order's quantity keeps its `seq`; a cancel/replace gives it a new
  one, exactly like queue priority. `last_seq()` returns the most recent value.
* **A read-only queue walk**: `for_each_level`, `for_each_in_level`,
  `queue_entry(id)` and `level_qty(side, px)`. None of them mutate or allocate.
* **`lobster::Replayer::resolve(id, side, px)`**: returns the book order that a
  type 2/3/4 row acts on. That is the id itself, or the synthetic order seeded
  from snapshot 0 for liquidity that predates the file.
* **`lobster::Row::time_ns`**: the timestamp parsed exactly from text into
  integer nanoseconds. Going through a `double` could be off by a nanosecond.

### Deviation from the plan: strategy orders are not in the book's FIFO

The plan proposed putting strategy orders into the book's level FIFOs, tagged
`owner = Strategy`. I did not do that. Instead, each resting strategy order
records `after_seq`, the book's `last_seq()` at the moment it arrived.

* A historical order X is ahead of the strategy order iff `seq(X) <= after_seq`.
* Queue position is therefore the same as with a FIFO insert.

The reason is the plan's own no-impact requirement. Inside the book, a
strategy order would interact with history:

* a later historical limit that crosses it would match against it and change
  the replayed book;
* level totals and `depth()` would include it, so parity checks would need
  filtering.

Keeping strategy orders outside the book makes the no-impact property hold by
construction, and the parity test then checks that it does (see validation).
Since no owner tag is needed, none was added.

## Event loop and time

```
while there is market data or a pending event at or before the last message:
    take market data if its ts <= the earliest pending event's ts   (market data wins ties)
        exchange.before_md(e)    # fills against the pre-event queue state
        replayer.apply(row)      # the existing engine, by historical order id
        exchange.after_md(e)     # own_taken bookkeeping
        strategy.on_market_data
    else pop the earliest pending event
        OrderArrive -> exchange (new/cancel)
        Deliver     -> strategy.on_order_event   (ack, reject, fill, cancelled)
        Timer       -> strategy.on_timer
```

* Time is int64 nanoseconds after midnight.
* Pending events are ordered by the key `(ts, priority, seq)`, with priority
  market data < order arrival < delivery < timer. `seq` is a global counter,
  so no two events ever compare equal.
* Historical messages stream from a vector. Only events the simulator creates
  go into the heap, which therefore holds just a handful of in-flight items.
* The session ends at the last message. In-flight acks and fills are still
  delivered, so the strategy's view ends up consistent. Arrivals still in
  flight and timers are dropped, and so is anything the strategy sends after
  the close. Both are counted in `summary.json` (`dropped_at_end`,
  `ignored_after_close`).
* Strategies are a template parameter (`Simulator<MarketMakerStrategy>`), not
  a virtual interface. `on_start` is optional and detected with `requires`.

## Latency

| Delay | Applied to | Config key |
|---|---|---|
| feed + processing | market event → the order it triggers leaving | `latency_ns.feed`, `latency_ns.proc` |
| order entry | strategy order/cancel → exchange arrival | `latency_ns.to_exchange` |
| return path | ack/reject/fill/cancel → strategy | `latency_ns.from_exchange` |
| jitter | added to entry and return | `latency_ns.jitter = {kind: uniform, max}` |

* The strategy runs at exchange time `t` on the book as it stands at `t`. Its
  orders arrive at `t + feed + proc + to_exchange (+ jitter)`. Seeing a stale
  book and acting immediately is equivalent, because the order reaches the
  exchange at the same moment either way.
* `feed` applies only to actions triggered by market data. Actions triggered
  by timers or order events pay `proc + to_exchange`.
* Each direction is clamped to be FIFO, so latency never reorders one
  strategy's own messages.
* The exchange timestamps orders by arrival. Every historical order stamped
  before the arrival time is ahead in the queue.
* `OrderApi::position()` is the strategy's own view, updated when fills are
  delivered, so it lags the exchange by the return latency.
* Jitter uses `std::mt19937_64`, whose output is fixed by the standard. It
  does not use `std::uniform_int_distribution`, which differs between
  libstdc++ and libc++.

## Exchange: order handling and fills

**Arrival of a new strategy order:**

1. Validate it in this order:
   * quantity > 0;
   * price on the tick grid and inside the band;
   * not halted;
   * `max_order_size`;
   * `max_open_orders` (limit orders only);
   * worst-case position `|position + same-side open qty + qty| <= max_position`;
   * self-trade prevention against our own resting orders on the other side.
2. Ack it.
3. If it is marketable, take liquidity. Walk the opposite side of the
   historical book. At each level, the available quantity is the displayed
   quantity minus `own_taken[(side, px)]`. Fill as taker at the level price.
   Nothing is removed from the replayed book.
4. A limit remainder rests, with `after_seq = book.last_seq()`.
   `queue_ahead` is the displayed level quantity plus any of our own orders
   already at that price. IOC and market remainders are cancelled
   (`unfilled`).

**Fills of resting strategy orders by historical messages.** These are
evaluated in `before_md`, so the queue is still in its pre-event state:

| Historical event | Effect on resting strategy orders |
|---|---|
| Type 4: visible execution of X, qty q, price p, side s | `remaining = q · exec_volume_share`. (1) Trade-through, if enabled: our orders on side s priced better than p fill first, best price first. (2) Our orders at p with `after_seq < seq(X)` fill in FIFO order. Orders behind X get nothing. |
| Type 2/3: partial cancel or delete | No fill. If the order was ahead of us, we move up for free. |
| Type 1: new order | It joins behind us. No effect. |
| Type 5: hidden execution | Never fills (pessimistic). Counted as `hidden_volume`. |
| Type 6/7: cross or halt | All strategy orders are cancelled (`halt`). New orders are rejected (`halted`) until the next type 1–5 message. |

A historical execution whose order id cannot be resolved is treated as hitting
the very front of the queue, so it gives us no fill. These are counted as
`unknown_exec_refs`, and the count was 0 on the AAPL day.

**own_taken.** Our takes are remembered per displayed level, so two
back-to-back takes cannot fill against the same shares. Each historical
partial cancel, delete or execution at that level first reduces `own_taken`
by its size. It is then clamped to the level's displayed quantity. The shares
we took were the front of the queue, so history removes those first.

**Cancels.** A cancel that arrives after the order filled is rejected as
`too_late_to_cancel`. A cancel for an id that never rested (for example, one
that was rejected) gets `unknown_order`. There is no modify: a modify is
cancel plus new, and loses priority.

## Accounting

* All accounting is integer arithmetic in price units (1/10000 dollar).
* Mid prices are carried as `2·mid` so they stay integer.
* Fees are integers per share: taker $0.0030 = 30 units, maker rebate
  $0.0020 = 20 units. The config refuses values finer than $0.0001.

On each fill of signed quantity `q` at price `p` with fee `f`:

```
cash     -= q·p + f
position += q
```

The cost basis (`basis`) is the signed cost of the open position, and closing
trades realize PnL against average cost. Flipping through zero closes the old
position and opens the remainder at the fill price. By construction:

```
realized + unrealized − fees == equity − equity_0
unrealized = position·mid − basis
equity     = cash + position·mid
```

Tests check this identity after every fill of a random sequence.

## Outputs

All files go to `out_dir`. Prices are written with exact decimal formatting
from integers. No floating-point number reaches the three hashed files.

| File | Content |
|---|---|
| `orders.csv` | `ts,order_id,event,side,price,qty,detail`. Events: `sent`, `arrived`, `acked`, `rejected`, `cancel_sent`, `cancelled`, `partial`, `filled`. `detail` holds the order type, reason, or maker/taker. |
| `fills.csv` | `ts,order_id,side,price,qty,liquidity,fee,queue_ahead,arrived,mid,mid_1s,mid_10s` |
| `pnl.csv` | One row per second of simulated time: `cash,inventory,mid,realized_pnl,unrealized_pnl,fees,equity` |
| `summary.json` | Net, gross, spread-capture and inventory PnL; fees; fill rate; queue wait; inventory; adverse selection at 1 s and 10 s; cancel/fill counts; parity and invariant counts; events per second; SHA-256 of the three CSVs (`output_sha256` and per-file hashes) |
| `manifest.json` | Git commit and dirty flag, compiler and flags, SHA-256 of the inputs, rows, seed, full config |

Diagnostics for a fill at price `p`, side `s`, with mid `m`:

* `edge = s·(m(t) − p)`
* `adverse_Δ = edge − s·(m(t+Δ) − p)`

Spread-capture PnL is Σ edge·qty, and inventory PnL is gross minus spread
capture. The mid at a fill is the mid before the triggering message moved
the book.

**Determinism.** Same inputs and config give byte-identical CSVs:

* there are no wall-clock reads in the simulation path;
* the PRNG is seeded;
* all containers whose iteration order matters are ordered (`std::map`/`std::set`);
* nothing is written until the run ends.

`events_per_second` and `wall_seconds` in `summary.json` are the only
wall-clock values, and they are outside the hash.

## Strategies

| Name | Purpose |
|---|---|
| `null` | Does nothing. Used by `ob-sim check` and as the no-impact baseline. |
| `scripted` | Issues exactly the listed actions. Every hand-built scenario uses it. |
| `probe` | Buys `qty` at the best bid every `period` and cancels it `hold` later. Used for the Python cross-check. |
| `market_maker` | Joins the touch (or sits `offset_ticks` behind it), skews both quotes by inventory, and stops quoting a side at `max_position`. Quotes only when the spread is ≥ `min_spread_ticks`. Requotes at most once per `requote_min_ns`, tracks pending cancels, backs off after a reject (`reject_backoff_ns`), and flattens with IOCs at `flatten_at`. |
| `imbalance` | Takes when the top-L book imbalance exceeds ±θ, exits after `hold_ns`, then waits out a cooldown. |

The market maker's reject backoff was added after the fuzz test caught a
reject storm. The maker re-quoted on every `halted` reject, was rejected
again, and so on until the halt cleared. The simulator surfaced this; the
fix belongs in the strategy.

## Data: ITCH → LOBSTER

Free LOBSTER samples are no longer downloadable (see `docs/design.md`).
`ob_itch2lobster` builds LOBSTER-format message and orderbook files for one
symbol from the NASDAQ ITCH 5.0 sample:

| ITCH | LOBSTER type |
|---|---|
| A, F | 1 |
| X | 2 |
| D | 3 |
| U | 3 then 1 |
| E, C | 4 |
| P | 5 |
| Q | 6 |

The book is built from the start of the feed (04:00), so no seeding is
needed. The orderbook file comes from this project's own book. Parity against
it therefore checks the LOBSTER replay path (id resolution, reductions) and
the simulator's no-impact property. It is not an independent check of the
book itself; that check is the ITCH execution-matching validation in
`docs/design.md`.

```
gunzip -c data/itch_raw/01302019_head600M.gz | ./build/ob_itch2lobster --symbol AAPL --levels 10 --out data/AAPL_2019-01-30
```

The output is 328,382 rows, 04:00:00 to 09:58:57, with 18 out-of-band orders
skipped and 0 unknown references.

## CLI

```
ob-sim run   --config sim/configs/aapl_mm.json [--out DIR]
ob-sim sweep --config sim/configs/aapl_mm.json --param latency_ns.to_exchange --values 0,10000,50000,200000,1000000,5000000
ob-sim check --config sim/configs/aapl_mm.json        # null strategy, parity on every row, invariants
```

`sweep` takes any dotted config path. It writes one run directory per value
and a `sweep.csv` table.
