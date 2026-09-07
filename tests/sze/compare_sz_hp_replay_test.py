#!/usr/bin/env python3

"""Regression tests for the Shenzhen HP replay first-divergence comparator."""

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path


TOOL = Path(__file__).resolve().parents[2] / "tools" / "sze" / "compare_sz_hp_replay.py"


def metadata():
    return {
        "schema": "sz-hp-replay-v1",
        "factor_count": 2,
        "factor_names": ["factor_0", "factor_1"],
        "model_input_names": ["factor_1", "factor_0"],
    }


def event(index):
    return {
        "schema": "sz-hp-replay-event-v1",
        "event_index": index,
        "event": "observation",
        "instrument": "000001.SZ",
        "sequence": index,
        "adapter_ok": True,
        "mutation_ok": True,
        "mutation_applied": True,
        "available": True,
        "prediction_suppressed": False,
        "digest": "state={}".format(index),
        "pre_failure_digest": "",
        "sample_ready": True,
        "sample_reason": "none",
        "sample_sequence": index,
        "sample_time_ms": 34200000 + index,
        "factor_raw": [1.0, 2.0],
        "factor_model_order": [2.0, 1.0],
        "normalized_input": [0.5, -0.5],
        "prediction_raw": 1.25,
        "prediction_clamped": 1.25,
    }


def write_jsonl(path, records):
    path.write_text("".join(json.dumps(record, sort_keys=True) + "\n" for record in records))


def run_compare(expected, actual, extra_args=None):
    command = [sys.executable, str(TOOL), str(expected), str(actual)]
    if extra_args:
        command.extend(extra_args)
    completed = subprocess.run(command, check=False, universal_newlines=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    report = json.loads(completed.stdout)
    return completed.returncode, report


def expect_failure(expected_path, actual_path, expected_category, expected_field):
    return_code, report = run_compare(expected_path, actual_path)
    if return_code != 1:
        raise AssertionError("comparison unexpectedly passed: {}".format(report))
    if report.get("category") != expected_category or report.get("field") != expected_field:
        raise AssertionError("unexpected first-divergence report: {}".format(report))


def main():
    with tempfile.TemporaryDirectory(prefix="sz-hp-replay-compare-") as directory:
        root = Path(directory)
        expected_path = root / "expected.jsonl"
        actual_path = root / "actual.jsonl"
        expected_records = [metadata(), event(1), event(2)]
        write_jsonl(expected_path, expected_records)

        write_jsonl(actual_path, copy.deepcopy(expected_records))
        return_code, report = run_compare(expected_path, actual_path)
        if return_code != 0 or report.get("status") != "PASS":
            raise AssertionError("matching replay did not pass: {}".format(report))

        actual = copy.deepcopy(expected_records)
        actual[1]["digest"] = "divergent-state"
        write_jsonl(actual_path, actual)
        expect_failure(expected_path, actual_path, "exact", "digest")

        actual = copy.deepcopy(expected_records)
        actual[1]["sample_ready"] = False
        write_jsonl(actual_path, actual)
        expect_failure(expected_path, actual_path, "exact", "sample_ready")

        actual = copy.deepcopy(expected_records)
        actual[1]["factor_raw"][1] = 3.0
        write_jsonl(actual_path, actual)
        expect_failure(expected_path, actual_path, "floating", "factor_raw")

        actual = copy.deepcopy(expected_records)
        actual[1]["prediction_raw"] = -1.25
        write_jsonl(actual_path, actual)
        expect_failure(expected_path, actual_path, "floating", "prediction_raw")

        actual = copy.deepcopy(expected_records)
        actual[1]["prediction_suppressed"] = True
        write_jsonl(actual_path, actual)
        expect_failure(expected_path, actual_path, "exact", "prediction_suppressed")

        actual = copy.deepcopy(expected_records)
        actual[1], actual[2] = actual[2], actual[1]
        write_jsonl(actual_path, actual)
        expect_failure(expected_path, actual_path, "ordering", "event_index")

        actual = copy.deepcopy(expected_records[:-1])
        write_jsonl(actual_path, actual)
        expect_failure(expected_path, actual_path, "ordering", "event_count")

        actual = copy.deepcopy(expected_records)
        actual[1]["factor_raw"][0] += 5e-7
        write_jsonl(actual_path, actual)
        return_code, report = run_compare(expected_path, actual_path)
        if return_code != 0 or report.get("status") != "PASS":
            raise AssertionError("configured floating tolerance was not applied: {}".format(report))

    print("compare_sz_hp_replay_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
