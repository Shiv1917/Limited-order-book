# Design

## Overview

```
 messages ──► BasicBook::process(msg, EventBuffer&) ──► Ack / Trade / Cancel / Reject events
               │
               ├─ IdMap      OrderId → pool index        (FlatIdMap: open addressing)
               ├─ Storage    pool of Order structs        (PoolStorage: index-addressed arena)
               ├─ levels_[2] flat array, one Level per tick per side
               │             Level = {head, tail, count, total}  intrusive FIFO of pool indices
               ├─ bits_[2]   hierarchical occupancy bitset per side
               └─ best_[2]   cached best bid / best ask level index
```

`BasicBook<OrderT, StorageT, IdMapT>` is one algorithm parameterised by three
policies. Every row of the benchmark table is this same code with one policy
swapped (see `include/ob/book.hpp`, bottom), so measured differences are caused
by data structures alone, never by an accidental difference in logic.

The reference book (`ref/ref_book.*`) is a deliberately plain
`std::map<Price, std::list<Order>>` per side plus `std::unordered_map` for id
lookup. It is the specification: the differential fuzzer requires the fast
book to produce byte-identical event streams and identical queue state after
every message.

## Data structures

**Price levels.** A price maps to `idx = (price - base) / tick`, which must be in
`[0, levels)` and exactly on tick, otherwise the order is rejected
(`InvalidPrice`). Each side has `levels` `Level` records. A level holds `head`
and `tail` pool indices of an intrusive doubly-linked FIFO, the order count and
total quantity. Linking through 32-bit indices rather than pointers halves link
size and keeps the pool relocatable (it can grow by reallocation).

**Best bid/ask.** `best_[side]` caches the best occupied level index. Adding to
an empty level updates it with one comparison. When the best level empties
(cancel or full fill), the next best level comes from the occupancy bitset.

**Occupancy bitset.** `LevelBitset` is a 64-ary hierarchy: layer 0 has one bit per
level, and each bit of layer k+1 means "word w of layer k is non-zero".
`next(i)`/`prev(i)` climb until a word has a set bit in the right direction,
then descend with `ctz`/`clz`. The cost is at most 2·L word reads, where
L = ⌈log64(levels)⌉: 3 for 262,144 levels and 4 for 16.7M. It does not depend
on how many empty ticks separate two prices, which a linear scan of a sparse
book would.

**Order storage.** `PoolStorage<T>` is one `std::vector<T>` sized up front and
value-initialised, so every page is faulted in at construction. A free list is
threaded through `T::next`, so `alloc`/`free` are two loads and a store with LIFO
reuse (recently freed slots are cache-hot). If the pre-sized pool runs out it
doubles, which is off the hot path and covered by a test. `HeapStorage<T>` is the
naive comparison: `new`/`delete` per order behind the same index interface.

**Order lookup.** `FlatIdMap` uses open addressing with linear probing, a
power-of-two capacity, Fibonacci hashing (`id * 0x9E37…` then the top bits) and
a load factor ≤ 0.5. Each slot is 16 bytes (`{id, index}`), four per cache line.
Deletion uses **backward shift** (Knuth 6.4 Algorithm R) instead of tombstones.
Order flow is dominated by add/cancel churn, and tombstones would steadily
lengthen probe sequences.

**Order layout.** `OrderLoose` (80 B) is the realistic "first attempt". It has
64-bit quantities, an explicit price, `orig_qty`, `tif`, timestamp and owner,
with hot and cold fields interleaved and padded, so three of every four orders
straddle two cache lines. `OrderPacked` (32 B, `alignas(32)`) keeps only what
matching touches: `id`, `qty`, `next`, `prev`, `level`, `side`. Price is implied
by the level, and cold metadata would live in a parallel array if needed. Two
orders fit in a line, and an order never straddles one.

**Events.** The engine appends 32-byte POD `Event`s to a caller-owned
`EventBuffer` (pre-allocated; it only grows if one message emits more events
than its capacity, e.g. a sweep of thousands of orders). There are no virtual
callbacks and no allocation on the hot path. The caller decides what to do with
the batch.

## Complexity

With `n` resting orders, `L = ⌈log64(levels)⌉` bitset layers, and `k` = number of
resting orders a taker trades with:

| Operation | Fast book | Reference (`std::map` + `std::list`) |
|---|---|---|
| Add (rests, no cross) | **O(1)** expected: hash insert, level append, bitset set ≤ L words | O(log n) map lookup/insert + list append |
| Cancel | **O(1)** expected: hash lookup, unlink, bitset clear ≤ L words; if it was the best level, next-best search ≤ 2L words | O(log P) map erase if the level empties, else O(1) via the stored iterator |
| Modify (decrease) | **O(1)** expected, in place | O(1) via iterator |
| Modify (price change or increase) | cancel + add, O(1) expected | O(log n) |
| Match against k orders | **O(k)** plus ≤ 2L words per emptied level | O(k + emptied levels · log P) |
| Best bid/ask | **O(1)** (cached) | O(1) (`begin()`) |
| Next best level after the best empties | **bounded bitset scan: ≤ 2L word reads** | O(1) amortised (`begin()` after erase) |
| FOK feasibility | O(levels visited) | O(levels visited) |

"Expected" refers to the hash map. The bitset scan is bounded by the number of
layers, not by the price distance. The fast book's advantage over the
reference is mostly constant factors, not big-O: no allocation, no tree
rebalancing and no pointer chasing across scattered heap nodes. See
`benchmarks.md`.

## Matching semantics

* **Price-time priority.** A taker trades against the best opposite level first,
  FIFO within a level, at the **maker's** price (price improvement goes to the
  taker).
* **Order types.**
  * Limit (GTC) matches, then rests.
  * IOC matches, then cancels the remainder (`Cancel{Unfilled}`).
  * FOK first checks that enough quantity exists at acceptable prices. If not,
    it emits `Cancel{FokUnfillable}` and touches nothing; otherwise it executes
    in full.
  * Market orders have no price limit and never rest; the remainder is
    cancelled. A market order may also be FOK.
* **Validation order** (identical in both books, so rejects match exactly):
  * add: `qty == 0` → `InvalidQty`; then off-band/off-tick price →
    `InvalidPrice`; then a live order with the same id → `DuplicateId`
  * cancel: unknown id → `UnknownOrder`
  * modify: unknown id → `UnknownOrder`, then `qty == 0` → `InvalidQty`, then
    bad price → `InvalidPrice`. A rejected modify leaves the order untouched.
* **Event sequence.** Every accepted order gets an `Ack` first, then zero or more
  `Trade`s, then possibly a `Cancel` for an unfilled IOC/FOK/market remainder.
* **Modify semantics.**
  * Same price and new quantity ≤ remaining quantity: reduce in place and
    **keep time priority** (`Ack{Modified}`).
  * Price change, or quantity increase: cancel/replace that **loses priority**.
    The order is removed first, then re-entered as a GTC limit at the new
    price. It can therefore cross the spread and trade immediately, and it can
    never trade with its own former quantity.
  * New quantity is the new *remaining* quantity, not original size.
* **Self-crossing.** An aggressive order or modify that crosses the spread trades
  immediately. There is no participant identity, so there is no self-trade
  prevention (see limitations).

## Validation strategy

1. **Unit tests** (`tests/test_book.cpp`) run every edge case against all five
   books. Covered cases include:
   * partial fills, multi-level sweeps with gaps, and price-time ordering
   * IOC/FOK/market behaviour, plus market orders on an empty book
   * cancelling a filled order, and modifying a nonexistent order
   * modify keeping or losing priority, and a crossing modify
   * duplicate ids, zero quantities, and off-band/off-tick prices
   * band edges, pool growth, and a next-best search across a 200,000-tick gap
     (4-layer bitset)
2. **Differential fuzzer** (`tests/fuzz_common.hpp`, `ob_fuzz_diff`). Random
   streams of realistic and adversarial messages (narrow bands, invalid
   messages, sweeps) go to the reference and to all four fast stages. After
   every message it asserts identical events, identical top of book, and
   identical full queue contents. Every 64 messages it also runs the fast book's
   `validate()` invariant check: links, aggregates, bitset vs. levels, best
   cache, id map and pool live count. The last local run covered 20M messages
   across 1,000 seeds and exercised every event type and reject reason. A
   planted bug (quantity increase keeping priority) was caught at message 127.
3. **libFuzzer target** (`tests/libfuzzer_target.cpp`): coverage-guided
   differential fuzzing on Linux, run in CI under ASan+UBSan.
4. **Sanitizers in CI**: ASan+UBSan and TSan builds run the full test suite.
5. **Real data** (`tools/itch.*`, `tools/lobster.*`), described next.

## Real-data validation

### NASDAQ ITCH 5.0

The public sample files are still at
`https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/` (checked 2026-11-02). The parser
streams the gzip (`curl | gunzip | ob_replay itch`), so you only need to
download a prefix of the 4.7 GB file.

ITCH has no book snapshots, but every `E` message (execution of a displayed
order) says which resting order the exchange matched. For each `E` the replayer:

1. checks that this order is at the front of the queue at the best price in
   **our** book;
2. if it is, replays the execution as an aggressive IOC order on the opposite
   side and checks that our engine emits exactly one trade, against that same
   maker, for that size, at that price.

This tests price-time priority and level bookkeeping against what the real
exchange did. Other messages are applied directly:

* `X` (partial cancel) becomes a quantity decrease, which keeps priority.
* `D` (delete) becomes a cancel.
* `U` (replace) becomes cancel plus a new add (which loses priority).
* `C` (execution at a different price, e.g. in a cross) becomes a reduction.

`--diff` also runs the reference book side by side.

Results on 2019-01-30, 04:00 → 09:58 (first 600 MB of the file):

| Symbol | Engine msgs | Executions replayed as aggressor, trade matched | Not at front (applied as reduction) | Crossed adds | Unknown refs | Ref-vs-fast mismatches |
|---|---|---|---|---|---|---|
| AAPL | 324,144 | 18,467 (98.28%) | 324 (1.72%) | 0 | 0 | 0 |
| SPY | 452,639 | 15,378 (100.00%) | 0 | 0 | 0 | 0 |
| QQQ | 568,458 | 7,406 (99.92%) | 6 | 0 | 0 | 0 |

Of the 324 AAPL cases, 3 fall within one second of the opening cross. In the
other 321, the executed order was at the best price, but other displayed orders
were queued ahead of it in our book. 245 of those 321 happened between 09:30
and 09:31 (Apple reported earnings the previous evening, so the open was very
active), and the rest came in short bursts later. This report does **not**
claim to know the cause. Plausible explanations include NASDAQ order types whose
priority differs from their display time, such as orders re-ranked when
re-priced. Following up would mean tagging these orders by their `F`
attribution and the order type each came from.

### LOBSTER

Free LOBSTER sample files are no longer directly downloadable: lobsterdata.com
is now a single-page app with gated sample requests, as of 2026-11-02. The
parser and replayer are complete and tested on a hand-built fixture
(`tests/data/TEST_*`):

* Executions (type 4) are applied as reductions of the named order, so after
  every message the book must match LOBSTER's snapshot level by level.
* Liquidity that predates the file (present in snapshot 0 but with no message
  history) is seeded as one synthetic order per level, with row 0's effect
  undone. Later partial cancels or executions of unknown ids are taken from the
  seeded order at that price.
* A test checks that a one-share corruption of a snapshot is detected.

If you obtain LOBSTER data, run:
`ob_replay lobster --messages X_message_10.csv --orderbook X_orderbook_10.csv --levels 10`.

## Market-making backtest

`strategy/market_maker.*` quotes one bid and one ask of `quote_size` around a
reservation price, `mid − skew · (inventory / quote_size) · tick`. It joins the
touch, optionally steps inside spreads of three or more ticks, never crosses, and
stops quoting a side at `max_inventory`.

Its orders go into the same engine as the replayed AAPL stream:

* The aggressive flow in that stream is the real aggressor flow reconstructed from
  ITCH executions, so our quotes are filled only when price-time priority in the
  engine puts them ahead.
* Actions can be delayed by `latency_msgs` market messages.
* P&L is marked to mid, plus maker rebates (or taker fees when a delayed quote
  ends up crossing).
* Markout is measured `markout_horizon` messages after each fill.

The model has known limitations:

* Our orders have no market impact: historical participants don't react to them.
* Historical cancels and modifies are replayed with their original sizes.
* When our quote trades with an order that historically rested, that order's
  later cancel is rejected (counted as `rejected market msgs`). That is the
  counterfactual working as intended.

## Known limitations

* **Fixed price band per book.** Prices outside `[base, base + levels·tick)` are
  rejected. Real-data tools size the band from the data (ITCH: first price
  ±65,536 ticks; out-of-band stub quotes are counted and tracked so their later
  messages are skipped consistently). A production engine would recentre or use
  a sparse fallback for far-away levels.
* **Memory.** Level arrays are dense: 2 sides × `levels` × 24 B, which is 6 MB at
  131,072 levels. The pool is pre-sized (default 1M orders × 32 B = 32 MB) and
  pre-faulted.
* **Single instrument, single thread** per book. Multiple symbols means multiple
  books (shard by symbol).
* **No self-trade prevention, no hidden/iceberg/pegged orders**, no auctions.
  Market orders have no price protection collar.
* **Quantities are 32-bit per order** (level totals are 64-bit).
* **Pool growth** reallocates (rare, off the hot path, but a latency spike when
  it happens). Size `max_orders` for the peak.
