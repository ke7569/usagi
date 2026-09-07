#!/usr/bin/env python3

"""Compare versioned Shenzhen HP replay JSONL and report the first divergence."""

import argparse
import json
import math
import sys
from pathlib import Path


EXACT_FIELDS = (
    "event_index",
    "event",
    "instrument",
    "sequence",
    "adapter_ok",
    "mutation_ok",
    "mutation_applied",
    "available",
    "prediction_suppressed",
    "digest",
    "pre_failure_digest",
    "sample_ready",
    "sample_reason",
    "sample_sequence",
    "sample_time_ms",
)
FLOAT_FIELDS = (
    "factor_raw",
    "factor_model_order",
    "normalized_input",
    "prediction_raw",
    "prediction_clamped",
)


def load(path):
    metadata = None
    events = []
    for line_number, line in enumerate(Path(path).read_text().splitlines(), 1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(f"{path}:{line_number}: invalid JSON: {error}") from error
        if record.get("schema") == "sz-hp-replay-v1":
            metadata = record
        elif record.get("schema") == "sz-hp-replay-event-v1":
            events.append(record)
        else:
            raise ValueError(f"{path}:{line_number}: unknown replay schema")
    if metadata is None:
        raise ValueError(f"{path}: missing replay metadata")
    return metadata, events


def close(left, right, atol, rtol):
    if isinstance(left, list) or isinstance(right, list):
        if not isinstance(left, list) or not isinstance(right, list):
            return False, "one value is not an array"
        if len(left) != len(right):
            return False, f"length {len(left)} != {len(right)}"
        for index, (lhs, rhs) in enumerate(zip(left, right)):
            equal, detail = close(lhs, rhs, atol, rtol)
            if not equal:
                return False, f"index={index} {detail}"
        return True, ""
    if left is None or right is None:
        return (left is right), f"{left!r} != {right!r}"
    try:
        lhs = float(left)
        rhs = float(right)
    except (TypeError, ValueError):
        return False, f"non-numeric {left!r} != {right!r}"
    if math.isnan(lhs) and math.isnan(rhs):
        return True, ""
    if math.isclose(lhs, rhs, abs_tol=atol, rel_tol=rtol):
        return True, ""
    return False, f"left={lhs:.17g} right={rhs:.17g} atol={atol} rtol={rtol}"


def fail(event_index, category, field, detail, expected=None, actual=None):
    report = {
        "status": "FAIL",
        "first_divergence_event_index": event_index,
        "category": category,
        "field": field,
        "detail": detail,
    }
    if expected is not None:
        report["expected"] = expected
    if actual is not None:
        report["actual"] = actual
    print(json.dumps(report, sort_keys=True))
    return 1


def compare(expected_metadata, expected_events, actual_metadata, actual_events, atol, rtol):
    for field in ("factor_count", "factor_names", "model_input_names"):
        if expected_metadata.get(field) != actual_metadata.get(field):
            return fail(0, "metadata", field, "metadata differs",
                        expected_metadata.get(field), actual_metadata.get(field))

    common = min(len(expected_events), len(actual_events))
    for position in range(common):
        expected = expected_events[position]
        actual = actual_events[position]
        event_index = expected.get("event_index", position + 1)
        if actual.get("event_index") != event_index:
            return fail(event_index, "ordering", "event_index", "missing or reordered event",
                        event_index, actual.get("event_index"))
        for field in EXACT_FIELDS:
            if expected.get(field) != actual.get(field):
                return fail(event_index, "exact", field, "exact value differs",
                            expected.get(field), actual.get(field))
        for field in FLOAT_FIELDS:
            if field not in expected and field not in actual:
                continue
            equal, detail = close(expected.get(field), actual.get(field), atol, rtol)
            if not equal:
                return fail(event_index, "floating", field, detail)

    if len(expected_events) != len(actual_events):
        event_index = common + 1
        return fail(event_index, "ordering", "event_count", "missing or extra event",
                    len(expected_events), len(actual_events))

    print(json.dumps({
        "status": "PASS",
        "events": len(expected_events),
        "samples": sum(bool(event.get("sample_ready")) for event in expected_events),
        "atol": atol,
        "rtol": rtol,
    }, sort_keys=True))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("expected", type=Path)
    parser.add_argument("actual", type=Path)
    parser.add_argument("--atol", type=float, default=1e-6)
    parser.add_argument("--rtol", type=float, default=1e-5)
    args = parser.parse_args()
    try:
        expected_metadata, expected_events = load(args.expected)
        actual_metadata, actual_events = load(args.actual)
    except ValueError as error:
        print(f"error={error}", file=sys.stderr)
        return 2
    return compare(expected_metadata, expected_events,
                   actual_metadata, actual_events, args.atol, args.rtol)


if __name__ == "__main__":
    raise SystemExit(main())
