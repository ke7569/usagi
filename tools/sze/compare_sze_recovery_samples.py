#!/usr/bin/env python3

"""Compare direct and recoverable mix153060 sample CSV files."""

import argparse
import csv
import json
import math
from pathlib import Path


EXACT_FIELDS = (
    "instrument",
    "exchange_time_us",
    "app_sequence",
    "cut_index",
    "row_in_stock_day",
    "window_start_exchange_time_us",
    "window_start_app_sequence",
    "window_start_cut_index",
    "amount_trigger",
    "time_trigger",
    "change_trigger",
    "source",
)
IGNORED_FIELDS = {
    "local_time_us",
    "framework_receive_time",
    "model_latency_ns",
    "raw_prediction",
}


def load(path):
    with Path(path).open(newline="") as handle:
        return list(csv.DictReader(handle))


def close(left, right, atol, rtol):
    lhs = float(left)
    rhs = float(right)
    if math.isnan(lhs) and math.isnan(rhs):
        return True, 0.0
    error = abs(lhs - rhs)
    return math.isclose(lhs, rhs, abs_tol=atol, rel_tol=rtol), error


def fail(row, field, expected, actual, detail):
    print(json.dumps({
        "status": "FAIL",
        "row": row,
        "field": field,
        "expected": expected,
        "actual": actual,
        "detail": detail,
    }, sort_keys=True))
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("direct", type=Path)
    parser.add_argument("recovery", type=Path)
    parser.add_argument("--prediction-atol", type=float, default=1e-3)
    parser.add_argument("--factor-atol", type=float, default=1e-6)
    parser.add_argument("--rtol", type=float, default=1e-5)
    args = parser.parse_args()

    direct = load(args.direct)
    recovery = load(args.recovery)
    if len(direct) != len(recovery):
        return fail(0, "row_count", len(direct), len(recovery), "sample counts differ")
    if direct and set(direct[0]) != set(recovery[0]):
        return fail(0, "columns", sorted(direct[0]), sorted(recovery[0]),
                    "sample schemas differ")

    max_prediction_error = 0.0
    max_numeric_error = 0.0
    numeric_fields = [
        field for field in (direct[0].keys() if direct else ())
        if field not in EXACT_FIELDS and field not in IGNORED_FIELDS
    ]
    for index, (expected, actual) in enumerate(zip(direct, recovery), 1):
        for field in EXACT_FIELDS:
            if expected[field] != actual[field]:
                return fail(index, field, expected[field], actual[field],
                            "exact field differs")
        equal, error = close(expected["raw_prediction"], actual["raw_prediction"],
                             args.prediction_atol, args.rtol)
        max_prediction_error = max(max_prediction_error, error)
        if not equal:
            return fail(index, "raw_prediction", expected["raw_prediction"],
                        actual["raw_prediction"], "prediction tolerance exceeded")
        for field in numeric_fields:
            equal, error = close(expected[field], actual[field],
                                 args.factor_atol, args.rtol)
            max_numeric_error = max(max_numeric_error, error)
            if not equal:
                return fail(index, field, expected[field], actual[field],
                            "numeric/factor tolerance exceeded")

    print(json.dumps({
        "status": "PASS",
        "rows": len(direct),
        "prediction_atol": args.prediction_atol,
        "factor_atol": args.factor_atol,
        "max_prediction_error": max_prediction_error,
        "max_numeric_error": max_numeric_error,
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
