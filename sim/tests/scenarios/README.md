# Hand-built fill scenarios

Each `*.scn` file is one scenario, run with the scripted strategy. Sections:

* `[config]`: JSON merged over the scenario defaults (latency 0, large risk
  limits, default fees). `strategy.actions` lists the scripted orders.
* `[messages]`: LOBSTER message rows `time,type,id,size,price,direction`.
  The session ends at the last message, so scenarios whose actions come after
  the last real event end with a harmless far-away add (`id 999` at $9.00).
  Prices are dollars × 10000. The book starts empty.
* `[expected]`: the exchange-side event log, written by hand:
  `ts,order,event,side,price,qty,detail`. `order` is the strategy order's
  ordinal (0 for the first `send`, and so on). `sent`, `cancel_sent` and
  `arrived` rows are not listed.
* `[checks]` (optional): `key=value` lines for end-of-run state. Keys are
  `position`, `fees` and `cash`, all in price units.

Lines starting with `#` are comments.
