#!/usr/bin/env python3
"""Compare journal probe samples row by row, including every factor and identity."""
import argparse
import csv
import itertools
import json
import math


def compare(baseline, candidate, tolerance):
    rows = changed = 0
    maximum = [0.0] * 50
    violations = 0
    with open(baseline) as left, open(candidate) as right:
        for a, b in itertools.zip_longest(csv.reader(left), csv.reader(right)):
            if a is None or b is None or len(a) != 58 or len(b) != 58:
                raise ValueError("sample count or column count mismatch at row " + str(rows))
            if a[:8] != b[:8]:
                raise ValueError("sample identity mismatch at row " + str(rows))
            rows += 1
            for index, (x, y) in enumerate(zip(a[8:], b[8:])):
                x, y = float(x), float(y)
                if not math.isfinite(x) or not math.isfinite(y):
                    raise ValueError("nonfinite factor at row " + str(rows))
                difference = abs(x - y)
                changed += difference != 0.0
                maximum[index] = max(maximum[index], difference)
                violations += difference > tolerance
    return {"samples": rows, "factor_values": rows * 50,
            "changed_values": changed, "identity_equal": True,
            "max_absolute_error": max(maximum), "max_error_by_factor": maximum,
            "absolute_tolerance": tolerance, "violations": violations,
            "passed": rows > 0 and violations == 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--absolute-tolerance", type=float, default=0.0)
    args = parser.parse_args()
    if not math.isfinite(args.absolute_tolerance) or args.absolute_tolerance < 0:
        parser.error("absolute tolerance must be finite and nonnegative")
    result = compare(args.baseline, args.candidate, args.absolute_tolerance)
    print(json.dumps(result, sort_keys=True))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
