#!/usr/bin/env python3
"""Independent reference implementation of the probe-strategy fill model.

Written from the spec only; optimised for readability, not speed.
Book: per side, dict price -> list (FIFO). List entries are either a historical
order id (int) or an own-order marker ('OWN', index). Displayed quantities only
count historical orders.
"""
import argparse
import csv
import heapq
import sys

BID, ASK = 1, -1
EMPTY_ASK, EMPTY_BID = 9999999999, -9999999999


def parse_ns(s):
    sec, frac = s.split('.')
    assert len(frac) == 9, s
    return int(sec) * 10**9 + int(frac)


def fmt_ns(ns):
    return f"{ns // 10**9}.{ns % 10**9:09d}"


def fmt_px(px):
    return f"{px // 10000}.{px % 10000:04d}"


class Model:
    def __init__(self, qty):
        self.levels = {BID: {}, ASK: {}}   # side -> price -> FIFO list
        self.lvl_qty = {BID: {}, ASK: {}}  # side -> price -> displayed qty
        self.orders = {}                   # hist id -> [side, price, qty]
        self.own = {}                      # own index -> dict(price, leaves)
        self.own_taken = {}                # (side, price) -> qty
        self.halted = False
        self.qty = qty
        self.fills = []                    # (ts, idx, side, price, qty, liq)
        self.next_idx = 0

    # ---------- historical book ----------
    def displayed(self, side, price):
        return self.lvl_qty[side].get(price, 0)

    def best_bid(self):
        px = [p for p, q in self.lvl_qty[BID].items() if q > 0]
        return max(px) if px else None

    def top(self, side, n=10):
        px = [p for p, q in self.lvl_qty[side].items() if q > 0]
        px.sort(reverse=(side == BID))
        return [(p, self.lvl_qty[side][p]) for p in px[:n]]

    def _remove_entry(self, side, price, entry):
        lst = self.levels[side][price]
        lst.remove(entry)
        if not lst:
            del self.levels[side][price]

    def _add_qty(self, side, price, d):
        q = self.lvl_qty[side].get(price, 0) + d
        assert q >= 0
        if q == 0:
            self.lvl_qty[side].pop(price, None)
        else:
            self.lvl_qty[side][price] = q

    def apply_message(self, typ, oid, size, price, direc):
        if typ == 1:
            self.orders[oid] = [direc, price, size]
            self.levels[direc].setdefault(price, []).append(oid)
            self._add_qty(direc, price, size)
        elif typ in (2, 3, 4):
            o = self.orders.get(oid)
            if o is None:
                print(f"warning: unknown id {oid} type {typ}", file=sys.stderr)
                return
            side, opx, oqty = o
            dec = oqty if typ == 3 else min(size, oqty)
            o[2] -= dec
            self._add_qty(side, opx, -dec)
            if o[2] == 0:
                del self.orders[oid]
                self._remove_entry(side, opx, oid)
            key = (side, opx)
            if key in self.own_taken:
                t = max(0, self.own_taken[key] - size)
                t = min(t, self.displayed(side, opx))
                if t == 0:
                    del self.own_taken[key]
                else:
                    self.own_taken[key] = t

    # ---------- own-order fills from market data ----------
    def maker_fills(self, ts, oid, size, price, direc):
        if direc != BID:
            return
        remaining = size
        # Price-time priority among own orders: higher price first, then FIFO.
        for idx in sorted(self.own, key=lambda i: (-self.own[i]['price'], i)):
            if remaining <= 0:
                break
            o = self.own[idx]
            f = 0
            if o['price'] > price:
                f = min(remaining, o['leaves'])
            elif o['price'] == price:
                lst = self.levels[BID].get(price, [])
                if oid in lst and lst.index(('OWN', idx)) < lst.index(oid):
                    f = min(remaining, o['leaves'])
            if f > 0:
                remaining -= f
                o['leaves'] -= f
                self.fills.append((ts, idx, 'buy', o['price'], f, 'maker'))
                if o['leaves'] == 0:
                    self._drop_own(idx)

    def _drop_own(self, idx):
        o = self.own.pop(idx)
        self._remove_entry(BID, o['price'], ('OWN', idx))

    def on_message(self, ts, typ, oid, size, price, direc):
        if typ in (1, 2, 3, 4, 5):
            self.halted = False
        if typ == 4:
            self.maker_fills(ts, oid, size, price, direc)
        if typ in (6, 7):
            for idx in list(self.own):
                self._drop_own(idx)
            self.halted = True
            return
        self.apply_message(typ, oid, size, price, direc)

    # ---------- strategy / exchange events ----------
    def on_arrival(self, ts, idx, price):
        if self.halted:
            return
        remaining = self.qty
        for ap in sorted(p for p, q in self.lvl_qty[ASK].items() if q > 0):
            if ap > price or remaining == 0:
                break
            avail = self.displayed(ASK, ap) - self.own_taken.get((ASK, ap), 0)
            if avail > 0:
                f = min(avail, remaining)
                remaining -= f
                self.own_taken[(ASK, ap)] = self.own_taken.get((ASK, ap), 0) + f
                self.fills.append((ts, idx, 'buy', ap, f, 'taker'))
        if remaining > 0:
            self.own[idx] = {'price': price, 'leaves': remaining}
            self.levels[BID].setdefault(price, []).append(('OWN', idx))

    def on_cancel(self, ts, idx):
        if idx in self.own and self.own[idx]['leaves'] > 0:
            self._drop_own(idx)


def read_snap(row):
    v = list(map(int, row))
    asks, bids = [], []
    for i in range(10):
        ap, asz, bp, bsz = v[4 * i:4 * i + 4]
        if ap != EMPTY_ASK:
            asks.append((ap, asz))
        if bp != EMPTY_BID:
            bids.append((bp, bsz))
    return asks, bids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--messages', required=True)
    ap.add_argument('--orderbook')
    ap.add_argument('--out', required=True)
    ap.add_argument('--start', type=int, default=34200)
    ap.add_argument('--stop', type=int, default=35900)
    ap.add_argument('--period', type=int, default=10)
    ap.add_argument('--hold', type=int, default=5)
    ap.add_argument('--qty', type=int, default=100)
    ap.add_argument('--latency-ns', type=int, default=50000)
    ap.add_argument('--check-every', type=int, default=1)
    a = ap.parse_args()
    S, P, H, L = a.start * 10**9, a.period * 10**9, a.hold * 10**9, a.latency_ns

    m = Model(a.qty)
    events = []  # (time, seq, kind, payload)
    seq = 0
    k = 0
    while S + k * P < a.stop * 10**9:
        events.append((S + k * P, seq, 'timer', None)); seq += 1; k += 1
    heapq.heapify(events)

    def run_events_before(t):  # strictly earlier than t (ties go to market data)
        nonlocal seq
        while events and (t is None or events[0][0] < t):
            et, _, kind, pl = heapq.heappop(events)
            if kind == 'timer':
                bb = m.best_bid()
                if bb is not None:
                    idx = m.next_idx; m.next_idx += 1
                    heapq.heappush(events, (et + L, seq, 'arrive', (idx, bb))); seq += 1
                    heapq.heappush(events, (et + H + L, seq, 'cancel', idx)); seq += 1
            elif kind == 'arrive':
                m.on_arrival(et, *pl)
            else:
                m.on_cancel(et, pl)

    snap_f = open(a.orderbook) if a.orderbook else None
    snaps = csv.reader(snap_f) if snap_f else None
    checked = bad = n = 0
    with open(a.messages) as f:
        rows = list(csv.reader(f))
    for i, r in enumerate(rows):
        ts = parse_ns(r[0])
        typ, oid, size, price, direc = (int(x) for x in r[1:6])
        run_events_before(ts)
        m.on_message(ts, typ, oid, size, price, direc)
        n += 1
        if snaps is not None:
            srow = next(snaps)
            if i % a.check_every == 0 or i == len(rows) - 1:
                checked += 1
                asks, bids = read_snap(srow)
                if asks != m.top(ASK) or bids != m.top(BID):
                    bad += 1
                    if bad <= 5:
                        print(f"BOOK MISMATCH at msg {i+1} {r}", file=sys.stderr)
    run_events_before(None)
    if snaps is not None:
        print(f"book check: {checked} snapshots checked, {bad} mismatches")

    with open(a.out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['ts', 'order_index', 'side', 'price', 'qty', 'liquidity'])
        for ts, idx, side, px, q, liq in m.fills:
            w.writerow([fmt_ns(ts), idx, side, fmt_px(px), q, liq])
    mk = sum(1 for x in m.fills if x[5] == 'maker')
    print(f"messages={n} orders_sent={m.next_idx} fills={len(m.fills)} "
          f"maker={mk} taker={len(m.fills) - mk}")


if __name__ == '__main__':
    main()
