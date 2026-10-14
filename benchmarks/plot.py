#!/usr/bin/env python3
"""
Draw horizontal bar charts (SVG) from the benchmark CSVs.

Pure standard library on purpose: no matplotlib needed, and SVG renders
directly on GitHub, in light and dark mode.

usage: benchmarks/plot.py [results-dir]
"""
import csv
import html
import os
import sys
from collections import OrderedDict

PALETTE = ["#4e79a7", "#f28e2b", "#59a14f", "#e15759", "#76b7b2", "#b07aa1"]
TEXT = "#8b949e"  # readable on both white and dark backgrounds

CHARTS = [
    # file, benchmark, title, group rows by, better
    ("spawn.csv", "spawn", "Process creation: create + wait (µs, lower is better)",
     "parameter", "lower"),
    ("ipc.csv", "ipc_latency", "IPC round-trip latency, 64 B (µs, lower is better)",
     "parameter", "lower"),
    ("ipc.csv", "ipc_throughput", "IPC throughput (MiB/s, higher is better)",
     "parameter", "higher"),
    ("sync.csv", "sync", "Increments per second, all threads (millions, higher is better)",
     "variant", "higher"),
    ("fileio.csv", "fileio", "Sequential scan of a cached file (MiB/s, higher is better)",
     "parameter", "higher"),
]


def load(path, bench):
    with open(path, newline="") as f:
        return [r for r in csv.DictReader(f) if r["benchmark"] == bench]


def chart(rows, title, group_by, out_path):
    series_key = "parameter" if group_by == "variant" else "variant"
    groups = OrderedDict()
    series = []
    for r in rows:
        groups.setdefault(r[group_by], []).append(r)
        if r[series_key] not in series:
            series.append(r[series_key])
    vmax = max(float(r["value"]) for r in rows) or 1.0

    label_w, bar_h, gap, plot_w = 150, 16, 12, 420
    width = label_w + plot_w + 90
    y = 40
    parts = []
    for gname, grows in groups.items():
        top = y
        for r in grows:
            v = float(r["value"])
            color = PALETTE[series.index(r[series_key]) % len(PALETTE)]
            w = max(1.0, plot_w * v / vmax)
            parts.append(f'<rect x="{label_w}" y="{y}" width="{w:.1f}" height="{bar_h}" '
                         f'rx="2" fill="{color}"/>')
            parts.append(f'<text x="{label_w + w + 6:.1f}" y="{y + bar_h - 4}" '
                         f'font-size="11" fill="{TEXT}">{v:,.1f}</text>')
            y += bar_h + 2
        mid = (top + y) / 2 + 4
        parts.append(f'<text x="{label_w - 8}" y="{mid:.1f}" font-size="12" '
                     f'text-anchor="end" fill="{TEXT}">{html.escape(gname)}</text>')
        y += gap

    # legend
    lx, ly = label_w, y + 4
    for i, s in enumerate(series):
        parts.append(f'<rect x="{lx}" y="{ly}" width="10" height="10" '
                     f'fill="{PALETTE[i % len(PALETTE)]}"/>')
        parts.append(f'<text x="{lx + 14}" y="{ly + 9}" font-size="11" '
                     f'fill="{TEXT}">{html.escape(s)}</text>')
        lx += 24 + 7 * len(s)
        if lx > width - 120:
            lx, ly = label_w, ly + 16
    height = ly + 24

    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}" font-family="Helvetica, Arial, sans-serif">'
           f'<text x="10" y="22" font-size="14" font-weight="bold" fill="{TEXT}">'
           f'{html.escape(title)}</text>' + "".join(parts) + "</svg>\n")
    with open(out_path, "w") as f:
        f.write(svg)


def main():
    results = sys.argv[1] if len(sys.argv) > 1 else "results"
    for fname, bench, title, group_by, _ in CHARTS:
        path = os.path.join(results, fname)
        if not os.path.exists(path):
            continue
        rows = load(path, bench)
        if rows:
            out = os.path.join(results, bench + ".svg")
            chart(rows, title, group_by, out)
            print("wrote", out)


if __name__ == "__main__":
    main()
