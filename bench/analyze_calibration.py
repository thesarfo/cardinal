#!/usr/bin/env python3
"""Fit the cost model's constants to a calibration run, and report rank agreement.

Usage: bench/analyze_calibration.py [bench/results/calibration.csv]

Two questions:

1. What do a pair check and a row cost, on this machine? The join operators' measured time is fitted
   to the cost model's own shapes:
       nested loop:  pairs x check + output x row
       hash join:    (build + probe) x check + (build + output) x row
   with `check` and `row` unknown. Rows are weighted by 1 / time^2 so that a 0.01 ms join counts as
   much as a 100 ms one. Only the ratio row / check matters to the choice of join, so the answer is
   also given as the CostParams to use (per_row_cost kept at 0.01).

2. When the model prices three ways to run a join, does the cheapest one run fastest? Per query
   (one cell of the grid), the methods are ranked by estimated cost and by measured time.
   Reported: how often the cheapest-priced method is the fastest (or within 10% of it, since two
   times that close are noise), and the share of pairs of methods the model orders as the clock does.
"""

import csv
import itertools
import math
import sys
from collections import defaultdict

PATH = sys.argv[1] if len(sys.argv) > 1 else "bench/results/calibration.csv"
TIE = 0.10  # two times within 10% of each other are treated as a tie


def read(path):
    lines = [l for l in open(path) if not l.startswith("#")]
    return list(csv.DictReader(lines))


def solve2(a11, a12, a22, b1, b2):
    det = a11 * a22 - a12 * a12
    return (b1 * a22 - b2 * a12) / det, (a11 * b2 - a12 * b1) / det


def fit(rows):
    """Weighted least squares for (check, row) in ms per unit."""
    a11 = a12 = a22 = b1 = b2 = 0.0
    used = 0
    for r in rows:
        t = max(float(r["join_self_ms"]), 0.002)  # below this the clock is just noise
        if r["variant"] == "nested_loop":
            x_check, x_row = float(r["pairs"]), float(r["join_out_rows"])
        else:
            build, probe, out = float(r["build_rows"]), float(r["probe_rows"]), float(r["join_out_rows"])
            x_check, x_row = build + probe, build + out
        if x_check == 0 and x_row == 0:
            continue
        w = 1.0 / (t * t)
        a11 += w * x_check * x_check
        a12 += w * x_check * x_row
        a22 += w * x_row * x_row
        b1 += w * x_check * t
        b2 += w * x_row * t
        used += 1
    check, row = solve2(a11, a12, a22, b1, b2)
    return check, row, used


def cells(rows):
    out = defaultdict(list)
    for r in rows:
        out[r["query_id"]].append(r)
    return out


def agreement(rows, price):
    """price(row) -> a number to rank by. Returns (top1, top1_lenient, pairs_agree, pairs, cells)."""
    top1 = lenient = agree = pairs = n = 0
    for cell in cells(rows).values():
        priced = [(price(r), float(r["exec_ms"])) for r in cell]
        if any(math.isnan(p) for p, _ in priced):
            continue
        n += 1
        cheapest = min(priced, key=lambda pt: pt[0])
        fastest = min(t for _, t in priced)
        top1 += cheapest[1] == fastest
        lenient += cheapest[1] <= fastest * (1 + TIE)
        for (p1, t1), (p2, t2) in itertools.combinations(priced, 2):
            if abs(t1 - t2) <= TIE * min(t1, t2) or p1 == p2:
                continue  # a tie on either side says nothing about the order
            pairs += 1
            agree += (p1 < p2) == (t1 < t2)
    return top1, lenient, agree, pairs, n


def pct(a, b):
    return "n/a" if b == 0 else f"{100.0 * a / b:.0f}%"


def main():
    rows = read(PATH)
    check, row, used = fit(rows)
    print(f"{PATH}: {len(rows)} runs, {len(cells(rows))} queries\n")

    print(f"Fitted on {used} join runs: check = {check * 1e6:.2f} ns, row = {row * 1e6:.2f} ns")
    print(f"  row / check = {row / check:.2f}   (the default settings assume 0.01 / 0.0025 = 4.00)")
    per_check = 0.01 * check / row
    print(f"  as CostParams with per_row_cost = 0.01:  per_check_cost = {per_check:.5f}\n")

    def formula(r, c, w):
        if r["variant"] == "nested_loop":
            return c * float(r["pairs"]) + w * float(r["join_out_rows"])
        return c * (float(r["build_rows"]) + float(r["probe_rows"])) + w * (float(r["build_rows"]) + float(r["join_out_rows"]))

    errors = []
    for r in rows:
        t = max(float(r["join_self_ms"]), 0.002)
        errors.append(abs(math.log(max(formula(r, check, row), 1e-9) / t)))
    errors.sort()
    print(f"Fit quality: the typical join is predicted within a factor of {math.exp(errors[len(errors) // 2]):.2f} "
          f"(median), {math.exp(errors[int(len(errors) * 0.9)]):.2f} (90th percentile)\n")

    print("Does the cheapest-priced method run fastest?")
    header = f"  {'priced by':<42} {'cheapest is fastest':>20} {'within 10%':>11} {'pairs ordered right':>20}"
    print(header)
    for label, price in [
        ("the cost model in this file (est_cost)", lambda r: float(r["est_cost"]) if r["est_cost"] not in ("", "nan") else float("nan")),
        ("fitted constants, true row counts", lambda r: formula(r, check, row)),
        ("work done (rows_processed)", lambda r: float(r["rows_processed"])),
    ]:
        top1, lenient, agree, pairs, n = agreement(rows, price)
        print(f"  {label:<42} {pct(top1, n) + f' ({top1}/{n})':>20} {pct(lenient, n):>11} {pct(agree, pairs) + f' ({agree}/{pairs})':>20}")
    print()

    print("Where the cost model's choice was slower than the fastest method:")
    shown = 0
    for qid, cell in cells(rows).items():
        priced = [(float(r["est_cost"]), float(r["exec_ms"]), r["variant"]) for r in cell]
        cheapest = min(priced)
        fastest = min(priced, key=lambda p: p[1])
        if cheapest[1] > fastest[1] * (1 + TIE):
            shown += 1
            print(f"  {qid:<18} priced cheapest: {cheapest[2]:<17} {cheapest[1]:8.3f} ms   fastest: {fastest[2]:<17} {fastest[1]:8.3f} ms "
                  f"({cheapest[1] / fastest[1]:.1f}x slower)")
    if not shown:
        print("  none")


if __name__ == "__main__":
    main()
