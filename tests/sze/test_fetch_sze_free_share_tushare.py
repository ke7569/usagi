#!/usr/bin/env python3
from __future__ import print_function

import csv
import importlib.util
import os
import sys
import tempfile
import types
import unittest

import pandas as pd


SCRIPT = os.path.abspath(os.path.join(
    os.path.dirname(__file__), "..", "..", "tools", "sze", "fetch_sze_free_share_tushare.py"
))
SPEC = importlib.util.spec_from_file_location("fetch_sze_free_share_tushare", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FakePro(object):
    def trade_cal(self, **kwargs):
        return pd.DataFrame([{"cal_date": "20260401", "is_open": 1, "pretrade_date": "20260331"}])

    def daily_basic(self, **kwargs):
        return pd.DataFrame([
            {"ts_code": "000001.SZ", "trade_date": "20260331", "free_share": 816048.1215},
            {"ts_code": "000807.SZ", "trade_date": "20260331", "free_share": 200781.2153},
            {"ts_code": "302001.SZ", "trade_date": "20260331", "free_share": 10.0},
            {"ts_code": "600000.SH", "trade_date": "20260331", "free_share": 10.0},
            {"ts_code": "002001.SZ", "trade_date": "20260331", "free_share": 0.0},
        ])


class FetchSzeFreeShareTushareTest(unittest.TestCase):
    def test_writes_v04_unit_and_filters_universe(self):
        fake = types.ModuleType("tushare")
        fake.pro_api = lambda token: FakePro()
        previous_module = sys.modules.get("tushare")
        previous_token = os.environ.get("TUSHARE_TOKEN")
        sys.modules["tushare"] = fake
        os.environ["TUSHARE_TOKEN"] = "test-token"
        try:
            with tempfile.TemporaryDirectory() as root:
                output = os.path.join(root, "free_share.csv")
                MODULE.main([
                    "--target-date", "20260401",
                    "--output", output,
                ])
                with open(output, encoding="utf-8-sig", newline="") as handle:
                    rows = list(csv.DictReader(handle))
                self.assertEqual([row["code"] for row in rows], ["000001.SZ", "000807.SZ"])
                self.assertEqual(rows[0]["date"], "20260401")
                self.assertEqual(rows[0]["source_date"], "20260331")
                self.assertEqual(rows[0]["unit"], "万股")
                self.assertEqual(rows[0]["free_share"], "816048.1215")
        finally:
            if previous_module is None:
                sys.modules.pop("tushare", None)
            else:
                sys.modules["tushare"] = previous_module
            if previous_token is None:
                os.environ.pop("TUSHARE_TOKEN", None)
            else:
                os.environ["TUSHARE_TOKEN"] = previous_token


if __name__ == "__main__":
    unittest.main()
