#!/usr/bin/env python3

"""Regression tests for direct/recovery mix153060 sample comparison."""

import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


TOOL = Path(__file__).resolve().parents[2] / "tools" / "sze" / "compare_sze_recovery_samples.py"
FIELDS = [
    "instrument", "exchange_time_us", "local_time_us", "app_sequence",
    "cut_index", "row_in_stock_day", "window_start_exchange_time_us",
    "window_start_app_sequence", "window_start_cut_index", "last_price",
    "amount_trigger", "time_trigger", "change_trigger", "source",
    "framework_receive_time", "raw_prediction", "model_latency_ns",
    "factor_0", "factor_1",
]


def sample():
    return {
        "instrument": "000001",
        "exchange_time_us": "34200000000",
        "local_time_us": "34200000001",
        "app_sequence": "101",
        "cut_index": "1",
        "row_in_stock_day": "1",
        "window_start_exchange_time_us": "34199900000",
        "window_start_app_sequence": "99",
        "window_start_cut_index": "0",
        "last_price": "10.25",
        "amount_trigger": "1",
        "time_trigger": "0",
        "change_trigger": "0",
        "source": "88",
        "framework_receive_time": "1000000",
        "raw_prediction": "0.125",
        "model_latency_ns": "5000",
        "factor_0": "1.5",
        "factor_1": "-2.5",
    }


def write_csv(path, row):
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerow(row)


def compare(direct, recovery):
    completed = subprocess.run(
        [sys.executable, str(TOOL), str(direct), str(recovery)],
        check=False, universal_newlines=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE)
    return completed.returncode, json.loads(completed.stdout)


def main():
    with tempfile.TemporaryDirectory(prefix="sze-recovery-samples-") as directory:
        root = Path(directory)
        direct = root / "direct.csv"
        recovery = root / "recovery.csv"
        expected = sample()
        write_csv(direct, expected)

        within_tolerance = dict(expected)
        within_tolerance["raw_prediction"] = "0.1259"
        within_tolerance["local_time_us"] = "99999999999"
        within_tolerance["model_latency_ns"] = "900000"
        write_csv(recovery, within_tolerance)
        return_code, report = compare(direct, recovery)
        assert return_code == 0 and report["status"] == "PASS", report

        prediction_mismatch = dict(expected)
        prediction_mismatch["raw_prediction"] = "0.1261"
        write_csv(recovery, prediction_mismatch)
        return_code, report = compare(direct, recovery)
        assert return_code == 1 and report["field"] == "raw_prediction", report

        factor_mismatch = dict(expected)
        factor_mismatch["factor_0"] = "1.6"
        write_csv(recovery, factor_mismatch)
        return_code, report = compare(direct, recovery)
        assert return_code == 1 and report["field"] == "factor_0", report

    print("compare_sze_recovery_samples_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
