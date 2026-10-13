#!/usr/bin/env python3
"""Plots for E3 (every join order of a four-table query) and E6 (planning time by tables).

Reads bench/results/e3.csv and bench/results/plan.csv, writes e3.svg and plan.svg next to
them. Standard library only. Usage: bench/plot_join_order.py [results-dir]
"""
import csv
import math
import sys
from pathlib import Path

GREY = "#8a8a8a"
COLORS = {"left-deep": "#4a7fb5", "bushy": "#c9743a", "greedy": "#6a9955"}


def rows(path):
    with open(path) as f:
        return list(csv.DictReader(line for line in f if not line.startswith("#")))


def text(x, y, s, size=11, anchor="start", weight=400, fill=GREY):
    s = s.replace("&", "&amp;").replace("<", "&lt;")
    return f'<text x="{x}" y="{y}" fill="{fill}" font-size="{size}" text-anchor="{anchor}" font-weight="{weight}">{s}</text>'


def log_axis(lo, hi, top, bottom, left, right, fmt):
    """Gridlines at each power of ten from lo to hi. Returns the y-mapper and the svg."""
    span = math.log10(hi) - math.log10(lo)

    def y_of(v):
        return bottom - (math.log10(v) - math.log10(lo)) / span * (bottom - top)

    out = []
    p = math.ceil(math.log10(lo))
    while 10 ** p <= hi:
        y = y_of(10 ** p)
        out.append(f'<line x1="{left}" y1="{y:.1f}" x2="{right}" y2="{y:.1f}" stroke="{GREY}" stroke-width="0.5"/>')
        out.append(text(left - 8, y + 4, fmt(10 ** p), anchor="end"))
        p += 1
    return y_of, out


def ms_label(v):
    return f"{v:g} ms"


def plot_e3(src, dst):
    data = [r for r in rows(src)]
    orders = sorted((r for r in data if r["variant"] == "as_written"), key=lambda r: float(r["exec_ms"]))
    chosen = next(r for r in data if r["variant"] == "chosen")
    written = next(r for r in orders if r["order"] == "t1-t2-t0-t3")
    best = float(orders[0]["exec_ms"])

    w, h, left, right, top, bottom = 760, 400, 80, 740, 60, 330
    ys, grid = log_axis(0.1, 1000, top, bottom, left, right, ms_label)
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" font-family="Inter, system-ui, sans-serif">']
    svg.append(text(20, 28, "Time to run each of the 24 left-deep orders of a four-table join (log scale)", 13, weight=600))
    svg.append(text(20, 46, "Bars are sorted by time. Orange: the order the search chose. Red: the query as written.", 11))
    svg += grid
    gap = (right - left) / len(orders)
    for i, r in enumerate(orders):
        t = float(r["exec_ms"])
        color = "#c9743a" if r["order"] == chosen["order"] else "#c0504d" if r is written else "#4a7fb5"
        x = left + i * gap + gap * 0.15
        y = ys(max(t, 0.1))
        svg.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{gap * 0.7:.1f}" height="{bottom - y:.1f}" fill="{color}"/>')
    svg.append(text(left, bottom + 22, f"best {best:.2f} ms", 11))
    svg.append(text(right, bottom + 22, f"worst {float(orders[-1]['exec_ms']):.0f} ms", 11, anchor="end"))
    svg.append(text(w / 2, bottom + 22,
                    f"chosen {float(chosen['exec_ms']):.1f} ms, written {float(written['exec_ms']):.0f} ms", 11, anchor="middle"))
    svg.append("</svg>")
    Path(dst).write_text("\n".join(svg) + "\n")


def plot_plan(src, dst):
    data = [r for r in rows(src) if r["shape"] == "clique"]
    by_table = {}  # the greedy rows appear once under left-deep and once under bushy: keep the faster
    for r in data:
        key = (r["search"], int(r["tables"]))
        by_table[key] = min(by_table.get(key, math.inf), float(r["plan_ms"]))
    series = {}
    for (name, t), ms in sorted(by_table.items(), key=lambda kv: kv[0][1]):
        series.setdefault(name, []).append((t, ms))

    w, h, left, right, top, bottom = 760, 400, 80, 720, 60, 330
    lo_t, hi_t = 2, 14
    ys, grid = log_axis(0.001, 100, top, bottom, left, right, ms_label)

    def x_of(t):
        return left + (t - lo_t) / (hi_t - lo_t) * (right - left)

    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" font-family="Inter, system-ui, sans-serif">']
    svg.append(text(20, 28, "Time to choose a join order, every table joined to every other (log scale)", 13, weight=600))
    svg += grid
    for t in range(lo_t, hi_t + 1, 2):
        svg.append(text(x_of(t), bottom + 20, str(t), anchor="middle"))
    svg.append(text((left + right) / 2, bottom + 40, "tables", anchor="middle"))
    for name, pts in series.items():
        color = COLORS.get(name, GREY)
        if name == "greedy":  # one line in the data, but it applies to both searches above 10 tables
            pts = [(10, dict(series["left-deep"])[10])] + pts
        path = " ".join(f"{'M' if i == 0 else 'L'}{x_of(t):.1f},{ys(ms):.1f}" for i, (t, ms) in enumerate(pts))
        svg.append(f'<path d="{path}" fill="none" stroke="{color}" stroke-width="2"/>')
        t, ms = pts[-1]
        svg.append(text(x_of(t) + 6, ys(ms) + 4, name, 11, fill=color))
    svg.append(text(x_of(10), ys(0.001) - 8, "full search stops at 10", 11, anchor="middle"))
    svg.append(f'<line x1="{x_of(10):.1f}" y1="{top}" x2="{x_of(10):.1f}" y2="{bottom}" stroke="{GREY}" stroke-width="0.5" stroke-dasharray="4 4"/>')
    svg.append("</svg>")
    Path(dst).write_text("\n".join(svg) + "\n")


if __name__ == "__main__":
    base = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "results"
    plot_e3(base / "e3.csv", base / "e3.svg")
    plot_plan(base / "plan.csv", base / "plan.svg")
    print(f"wrote {base / 'e3.svg'} and {base / 'plan.svg'}")
