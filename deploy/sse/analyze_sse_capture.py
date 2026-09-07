#!/usr/bin/env python3
"""Summarize local arrival timing between SSE snapshot and tick datagrams."""

from __future__ import print_function

import argparse
import json
import math
import sys


def percentile(values, q):
    if not values:
        return None
    values = sorted(values)
    index = int(round((len(values) - 1) * q))
    return values[index]


def load_rows(paths):
    rows = []
    for path in paths:
        with open(path) as source:
            for line_no, line in enumerate(source, 1):
                if not line.strip():
                    continue
                try:
                    row = json.loads(line)
                    if "ts_ns" in row and "channel" in row:
                        rows.append(row)
                except ValueError as exc:
                    print("skip malformed line %s:%d: %s" % (path, line_no, exc), file=sys.stderr)
    return rows


def summarize(rows):
    by_channel = {}
    for row in rows:
        by_channel.setdefault(row["channel"], []).append(row)
    print("rows=%d channels=%d" % (len(rows), len(by_channel)))
    for channel in sorted(by_channel):
        values = sorted(by_channel[channel], key=lambda row: row["ts_ns"])
        gaps = [b["ts_ns"] - a["ts_ns"] for a, b in zip(values, values[1:])]
        print("channel=%s rows=%d first_ts_ns=%s last_ts_ns=%s median_gap_us=%s p99_gap_us=%s" % (
            channel, len(values), values[0]["ts_ns"], values[-1]["ts_ns"],
            "%.3f" % (percentile(gaps, 0.5) / 1000.0) if gaps else "NA",
            "%.3f" % (percentile(gaps, 0.99) / 1000.0) if gaps else "NA"))

    snapshot = [row for channel, values in by_channel.items()
                if "snapshot" in channel for row in values]
    tick = [row for channel, values in by_channel.items()
            if "tick" in channel or "trade" in channel for row in values]
    if not snapshot or not tick:
        print("relation=insufficient_snapshot_or_tick_rows")
        return
    tick_ts = sorted(row["ts_ns"] for row in tick)
    deltas = []
    cursor = 0
    for row in sorted(snapshot, key=lambda item: item["ts_ns"]):
        target = row["ts_ns"]
        while cursor + 1 < len(tick_ts) and tick_ts[cursor + 1] <= target:
            cursor += 1
        candidates = [tick_ts[cursor]]
        if cursor + 1 < len(tick_ts):
            candidates.append(tick_ts[cursor + 1])
        deltas.append(min(abs(value - target) for value in candidates))
    print("relation=snapshot_nearest_tick samples=%d median_abs_delta_us=%.3f p95_abs_delta_us=%.3f p99_abs_delta_us=%.3f" % (
        len(deltas), percentile(deltas, 0.5) / 1000.0,
        percentile(deltas, 0.95) / 1000.0, percentile(deltas, 0.99) / 1000.0))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("jsonl", nargs="+", help="one or more observer JSONL files")
    args = parser.parse_args()
    rows = load_rows(args.jsonl)
    if not rows:
        print("no valid rows", file=sys.stderr)
        return 1
    summarize(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
