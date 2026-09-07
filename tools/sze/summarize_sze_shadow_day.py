#!/usr/bin/env python3
"""Produce a deterministic post-close summary for all SZE shadow shards."""

import argparse
import csv
import json
import math
import os
from collections import Counter, defaultdict


def percentile(values, p):
    if not values:
        return None
    ordered = sorted(values)
    index = int(math.ceil((len(ordered) - 1) * p))
    return ordered[index]


def to_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def rows(path):
    if not os.path.isfile(path):
        return []
    with open(path, newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True,
                        help=".../log/sze_all_YYYYMMDD")
    parser.add_argument("--day", required=True)
    parser.add_argument("--shards", type=int, default=8)
    parser.add_argument("--output")
    args = parser.parse_args()
    summary = {"trading_day": int(args.day), "shards": [], "by_instrument": {}}
    instruments = defaultdict(lambda: Counter())
    for shard in range(args.shards):
        directory = os.path.join(args.root, "shard_{:02d}".format(shard))
        event_rows = rows(os.path.join(directory, "shard_{:02d}_events.csv".format(shard)))
        sample_rows = rows(os.path.join(directory, "shard_{:02d}_samples.csv".format(shard)))
        predictions = [to_float(row.get("raw_prediction")) for row in sample_rows]
        predictions = [value for value in predictions if value is not None]
        model_ns = [to_float(row.get("model_latency_ns")) for row in sample_rows]
        model_ns = [value for value in model_ns if value is not None]
        invalid = sum(row.get("source_continuity_valid") != "1" for row in sample_rows)
        for row in event_rows:
            item = instruments[row.get("instrument", "")]
            item["events"] += 1
            item[row.get("event_type", "unknown")] += 1
        for row in sample_rows:
            instruments[row.get("instrument", "")]["samples"] += 1
        summary["shards"].append({
            "shard": shard,
            "events": len(event_rows),
            "samples": len(sample_rows),
            "invalid_samples": invalid,
            "prediction_p50": percentile(predictions, 0.50),
            "prediction_p01": percentile(predictions, 0.01),
            "prediction_p99": percentile(predictions, 0.99),
            "model_latency_ns_p50": percentile(model_ns, 0.50),
            "model_latency_ns_p99": percentile(model_ns, 0.99),
        })
    summary["by_instrument"] = dict(instruments)
    output = args.output or os.path.join(args.root, "summary_{}.json".format(args.day))
    with open(output, "w", encoding="utf-8") as stream:
        json.dump(summary, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(output)


if __name__ == "__main__":
    main()
