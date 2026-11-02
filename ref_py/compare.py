#!/usr/bin/env python3
"""Compare C++ fills.csv with the Python reference fills (ordered, row by row)."""
import argparse
import csv
import difflib
import sys

OID_BASE = 1 << 40


def load_cpp(path):
    with open(path) as f:
        return [(r['ts'], int(r['order_id']) - OID_BASE, r['side'], r['price'],
                 int(r['qty']), r['liquidity']) for r in csv.DictReader(f)]


def load_py(path):
    with open(path) as f:
        return [(r['ts'], int(r['order_index']), r['side'], r['price'],
                 int(r['qty']), r['liquidity']) for r in csv.DictReader(f)]


def summary(rows):
    mk = sum(1 for r in rows if r[5] == 'maker')
    return f"{len(rows)} fills (maker={mk}, taker={len(rows) - mk}, qty={sum(r[4] for r in rows)})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cpp', required=True)
    ap.add_argument('--py', required=True)
    ap.add_argument('--max-diffs', type=int, default=20)
    a = ap.parse_args()
    c, p = load_cpp(a.cpp), load_py(a.py)
    print("C++:", summary(c))
    print("Py :", summary(p))
    sm = difflib.SequenceMatcher(a=c, b=p, autojunk=False)
    matched = sum(b.size for b in sm.get_matching_blocks())
    print(f"matched rows: {matched}; only in C++: {len(c) - matched}; only in Py: {len(p) - matched}")
    shown = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            continue
        if shown >= a.max_diffs:
            print("... more differences omitted")
            break
        shown += 1
        print(f"--- diff #{shown} ({tag}) C++ rows {i1+1}-{i2}, Py rows {j1+1}-{j2}")
        if i1 > 0:
            print("  context  :", ','.join(map(str, c[i1 - 1])))
        for r in c[i1:i2]:
            print("  C++ only :", ','.join(map(str, r)))
        for r in p[j1:j2]:
            print("  Py  only :", ','.join(map(str, r)))
        if i2 < len(c):
            print("  context  :", ','.join(map(str, c[i2])))
    sys.exit(0 if c == p else 1)


if __name__ == '__main__':
    main()
