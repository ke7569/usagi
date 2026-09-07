#!/usr/bin/env python3

from __future__ import print_function

import csv
import datetime
import hashlib
import importlib.util
import json
import math
import os
import stat
import subprocess
import sys
import tempfile
import unittest


SCRIPT = os.path.abspath(os.path.join(
    os.path.dirname(__file__), "..", "..", "tools", "sze", "generate_sze_live_config.py"
))
SPEC = importlib.util.spec_from_file_location("generate_sze_live_config", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GenerateSzeLiveConfigTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        self.daily = os.path.join(self.root, "daily")
        self.output = os.path.join(self.root, "output")
        os.makedirs(self.daily)
        self.model = os.path.join(self.root, "mix.bin")
        with open(self.model, "wb") as handle:
            handle.write(b"accepted mix153060 test model")
        self.model_hash = hashlib.sha256(b"accepted mix153060 test model").hexdigest()
        with open(self.model + ".json", "w") as handle:
            json.dump({"binary_sha256": self.model_hash}, handle)

        self.template = os.path.join(self.root, "template.json")
        with open(self.template, "w") as handle:
            json.dump({
                "strategy_name": "test_sze",
                "name": "ZStrategy",
                "market": "SZ",
                "orderbook_mode": "hp-shadow",
                "model_path": self.model,
                "mix153060_model_sha256": self.model_hash,
                "md_source_index": [88],
                "td_source_index": [180],
                "order_data_source": "/tmp/replay-order.csv",
                "trade_data_source": "/tmp/replay-trade.csv",
                "replay_window": {"start": "09:30"},
                "instrument_id": ["000001"],
                "his_amt": [1.0],
                "static_position": [1200],
                "last_position": [100],
                "ins_params": {
                    "000001.SZ": {"static_position": 1200, "last_position": 100}
                },
                "global_params": {"offset": 0.8},
            }, handle)

        self.main_conf = os.path.join(self.root, "main.conf")
        with open(self.main_conf, "w") as handle:
            json.dump({
                "vmd": [{"source": 88, "lib": "md.so"}],
                "vtd": [{"source": 180, "lib": "td.so"}],
                "vstr": [{"lib": "old.so", "config": "old.json"}],
            }, handle)
        self.target = datetime.date(2026, 7, 22)

    def tearDown(self):
        self.temp.cleanup()

    def write_days(self, amount_field="amount_cny", missing_target_limit=False):
        dates = [self.target - datetime.timedelta(days=offset) for offset in range(21, 0, -1)]
        dates.append(self.target)
        rates = []
        for index, date in enumerate(dates):
            path = os.path.join(self.daily, "sze_daily_{}.csv".format(date.strftime("%Y%m%d")))
            rate = 0.0005 * float(index + 1)
            if date < self.target:
                rates.append(rate)
            fieldnames = [
                "date", "code", "close", "pre_close", amount_field,
                "upper_limit", "lower_limit",
            ]
            amount = float((index + 1) * 10000)
            if amount_field == "amount_10k_cny":
                amount /= 10000.0
            upper = "" if missing_target_limit and date == self.target else 11.0
            with open(path, "w", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=fieldnames)
                writer.writeheader()
                writer.writerow({
                    "date": date.strftime("%Y%m%d"),
                    "code": "000001.SZ",
                    "close": "" if date == self.target else 10.0 * math.exp(rate),
                    "pre_close": 10.0,
                    amount_field: amount,
                    "upper_limit": upper,
                    "lower_limit": 9.0,
                })
        return rates

    def args(self, extra=None):
        argv = [
            "--target-date", self.target.strftime("%Y%m%d"),
            "--template-json", self.template,
            "--daily-dir", self.daily,
            "--output-dir", self.output,
            "--main-conf-template", self.main_conf,
            "--strategy-library", "./libt0_strategy_sze.so",
            "--history-amount-days", "3",
        ]
        if extra:
            argv.extend(extra)
        return MODULE.parse_args(argv)

    def test_generates_causal_static_values_and_atomic_links(self):
        rates = self.write_days()
        first = MODULE.run(self.args())
        config_path = first["published"]["json"]
        with open(config_path) as handle:
            config = json.load(handle)
        params = config["ins_params"]["000001.SZ"]
        expected_amount = (190000.0 + 200000.0 + 210000.0) / 3.0
        selected_returns = rates[-20:]
        mean = sum(selected_returns) / len(selected_returns)
        expected_vol = math.sqrt(
            sum((value - mean) ** 2 for value in selected_returns) /
            (len(selected_returns) - 1)
        )
        self.assertEqual(params["Date"], 20260722)
        self.assertEqual(params["Close"], 10.0)
        self.assertAlmostEqual(params["HistoryAmount"], expected_amount, places=12)
        self.assertAlmostEqual(params["HistoryVolatility20d"], expected_vol, places=15)
        self.assertEqual(params["HpUpperPrice"], 11.0)
        self.assertEqual(params["HpLowerPrice"], 9.0)
        self.assertNotIn("order_data_source", config)
        self.assertNotIn("trade_data_source", config)
        self.assertNotIn("replay_window", config)
        self.assertEqual(config["sz_orderbook_mode"], "hp-shadow")
        self.assertEqual(config["model_path"], self.model)
        self.assertNotIn("mix153060_model_artifact", config)
        self.assertNotIn("hp_model_artifact", config)
        self.assertEqual(os.readlink(os.path.join(
            self.output, "config_sze_mix153060_live_current.json"
        )), os.path.basename(config_path))
        self.assertEqual(stat.S_IMODE(os.stat(config_path).st_mode), 0o644)
        with open(first["published"]["conf"]) as handle:
            main_conf = json.load(handle)
        self.assertEqual(main_conf["vstr"][0]["config"], config_path)
        self.assertEqual(main_conf["vstr"][0]["lib"], "./libt0_strategy_sze.so")

        before_hash = MODULE._sha256(config_path)
        second = MODULE.run(self.args())
        self.assertEqual(before_hash, MODULE._sha256(second["published"]["json"]))

    def test_converts_amount_10k_cny(self):
        self.write_days(amount_field="amount_10k_cny")
        result = MODULE.run(self.args())
        params = result["config"]["ins_params"]["000001.SZ"]
        self.assertAlmostEqual(params["HistoryAmount"], 200000.0, places=12)

    def test_loads_free_share_in_wan_shares_and_records_audit(self):
        self.write_days()
        free_share = os.path.join(self.root, "free_share.csv")
        with open(free_share, "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=["date", "code", "free_share", "unit"])
            writer.writeheader()
            writer.writerow({
                "date": self.target.strftime("%Y%m%d"),
                "code": "000001.SZ",
                "free_share": "816048.1215",
                "unit": "万股",
            })
        result = MODULE.run(self.args(["--free-share-file", free_share]))
        params = result["config"]["ins_params"]["000001.SZ"]
        self.assertAlmostEqual(params["FreeShare"], 816048.1215, places=10)
        self.assertEqual(result["audit"]["free_share_input"]["unit"], "万股")
        self.assertAlmostEqual(result["audit"]["instruments"][0]["free_share"], 816048.1215)

    def test_model_hash_mismatch_fails_before_publication(self):
        self.write_days()
        with open(self.template) as handle:
            template = json.load(handle)
        template["mix153060_model_sha256"] = "0" * 64
        with open(self.template, "w") as handle:
            json.dump(template, handle)
        with self.assertRaises(MODULE.ConfigError):
            MODULE.run(self.args())
        self.assertFalse(os.path.exists(self.output))

        template["mix153060_model_sha256"] = self.model_hash
        with open(self.template, "w") as handle:
            json.dump(template, handle)
        with open(self.model + ".json", "w") as handle:
            json.dump({"binary_sha256": "0" * 64}, handle)
        with self.assertRaises(MODULE.ConfigError):
            MODULE.run(self.args())
        self.assertFalse(os.path.exists(self.output))

    def test_missing_model_sidecar_fails_before_publication(self):
        self.write_days()
        os.unlink(self.model + ".json")
        with self.assertRaises(MODULE.ConfigError):
            MODULE.run(self.args())
        self.assertFalse(os.path.exists(self.output))

    def test_invalid_template_source_index_fails_before_publication(self):
        self.write_days()
        with open(self.template) as handle:
            template = json.load(handle)
        template["md_source_index"] = [40000]
        with open(self.template, "w") as handle:
            json.dump(template, handle)
        with self.assertRaises(MODULE.ConfigError):
            MODULE.run(self.args())
        self.assertFalse(os.path.exists(self.output))

    def test_realtime_zero_position_template_fails_closed(self):
        self.write_days()
        with open(self.template) as handle:
            template = json.load(handle)
        template["static_position"] = [0]
        template["last_position"] = [0]
        template["ins_params"]["000001.SZ"]["static_position"] = 0
        template["ins_params"]["000001.SZ"]["last_position"] = 0
        with open(self.template, "w") as handle:
            json.dump(template, handle)
        with self.assertRaises(MODULE.ConfigError):
            MODULE.run(self.args(["--mode", "hp-realtime"]))
        self.assertFalse(os.path.exists(self.output))

    def test_shadow_allows_no_td_source_but_realtime_rejects_it(self):
        self.write_days()
        with open(self.template) as handle:
            template = json.load(handle)
        template["td_source_index"] = []
        with open(self.template, "w") as handle:
            json.dump(template, handle)
        with open(self.main_conf) as handle:
            main_conf = json.load(handle)
        main_conf["vtd"] = []
        with open(self.main_conf, "w") as handle:
            json.dump(main_conf, handle)

        shadow = MODULE.run(self.args())
        self.assertEqual(shadow["config"]["td_source_index"], [])

        with self.assertRaises(MODULE.ConfigError):
            MODULE.run(self.args(["--mode", "hp-realtime"]))

    def test_capture_prefix_expands_target_date(self):
        self.write_days()
        with open(self.template) as handle:
            template = json.load(handle)
        template["mix153060_capture"] = {
            "enabled": True,
            "prefix": "000001_{target_date}",
        }
        with open(self.template, "w") as handle:
            json.dump(template, handle)

        result = MODULE.run(self.args())
        self.assertEqual(
            result["config"]["mix153060_capture"]["prefix"],
            "000001_20260722",
        )

    def test_main_conf_source_mismatch_fails_before_publication(self):
        self.write_days()
        with open(self.main_conf) as handle:
            main_conf = json.load(handle)
        main_conf["vmd"][0]["source"] = 999
        with open(self.main_conf, "w") as handle:
            json.dump(main_conf, handle)
        with self.assertRaises(MODULE.ConfigError):
            MODULE.run(self.args())
        self.assertFalse(os.path.exists(self.output))

    def test_non_sze_strategy_library_fails_before_publication(self):
        self.write_days()
        with self.assertRaises(MODULE.ConfigError):
            MODULE.run(self.args([
                "--strategy-library", "./libt0_strategy_sse.so",
            ]))
        self.assertFalse(os.path.exists(self.output))

    def test_missing_limit_does_not_replace_current(self):
        self.write_days()
        first = MODULE.run(self.args())
        current = os.path.join(self.output, "config_sze_mix153060_live_current.json")
        old_target = os.readlink(current)
        old_hash = MODULE._sha256(first["published"]["json"])

        self.write_days(missing_target_limit=True)
        with self.assertRaises(MODULE.ConfigError):
            MODULE.run(self.args())
        self.assertEqual(os.readlink(current), old_target)
        self.assertEqual(MODULE._sha256(first["published"]["json"]), old_hash)

    def test_publication_error_restores_dated_artifacts_and_current_links(self):
        self.write_days()
        first = MODULE.run(self.args())
        published = first["published"]
        old_hashes = {
            key: MODULE._sha256(path)
            for key, path in published.items() if path is not None
        }
        current_paths = [
            os.path.join(self.output, "config_sze_mix153060_live_current.json"),
            os.path.join(self.output, "main_sze_mix153060_live_current.conf"),
            os.path.join(self.output, "manifest_sze_mix153060_live_current.json"),
        ]
        old_links = {path: os.readlink(path) for path in current_paths}

        with open(self.template) as handle:
            template = json.load(handle)
        template["global_params"]["offset"] = 1.25
        with open(self.template, "w") as handle:
            json.dump(template, handle)

        original_replace = MODULE._replace_symlink
        calls = [0]

        def fail_second_link(link_path, target_name):
            calls[0] += 1
            if calls[0] == 2:
                raise OSError("injected current-link failure")
            return original_replace(link_path, target_name)

        MODULE._replace_symlink = fail_second_link
        try:
            with self.assertRaises(OSError):
                MODULE.run(self.args())
        finally:
            MODULE._replace_symlink = original_replace

        for key, path in published.items():
            if path is not None:
                self.assertEqual(MODULE._sha256(path), old_hashes[key])
        for path in current_paths:
            self.assertEqual(os.readlink(path), old_links[path])

    def test_fewer_than_five_volatility_rows_emits_zero(self):
        old_target = self.target
        self.target = datetime.date(2026, 7, 5)
        dates = [self.target - datetime.timedelta(days=offset) for offset in range(3, 0, -1)]
        dates.append(self.target)
        for index, date in enumerate(dates):
            path = os.path.join(self.daily, "sze_daily_{}.csv".format(date.strftime("%Y%m%d")))
            with open(path, "w", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=[
                    "date", "code", "close", "pre_close", "amount_cny",
                    "upper_limit", "lower_limit",
                ])
                writer.writeheader()
                writer.writerow({
                    "date": date.strftime("%Y%m%d"),
                    "code": "000001",
                    "close": 10.0 + index,
                    "pre_close": 10.0,
                    "amount_cny": 100000.0 + index,
                    "upper_limit": 15.0,
                    "lower_limit": 5.0,
                })
        result = MODULE.run(self.args())
        value = result["config"]["ins_params"]["000001.SZ"]["HistoryVolatility20d"]
        self.assertEqual(value, 0.0)
        self.target = old_target

    def test_scheduled_shell_entry(self):
        self.write_days()
        wrapper = os.path.abspath(os.path.join(
            os.path.dirname(__file__), "..", "..", "tools", "sze", "run_sze_live_config_job.sh"
        ))
        wrapper_output = os.path.join(self.root, "wrapper-output")
        env = dict(os.environ)
        env.update({
            "SZE_CONFIG_PYTHON": sys.executable,
            "SZE_TARGET_DATE": self.target.strftime("%Y%m%d"),
            "SZE_TEMPLATE_JSON": self.template,
            "SZE_DAILY_DIR": self.daily,
            "SZE_OUTPUT_DIR": wrapper_output,
            "SZE_MD_SOURCE_INDEX": "88",
            "SZE_TD_SOURCE_INDEX": "180",
        })
        completed = subprocess.run(
            [wrapper], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True,
        )
        self.assertEqual(completed.returncode, 0, msg=completed.stderr)
        self.assertTrue(os.path.islink(os.path.join(
            wrapper_output, "config_sze_mix153060_live_current.json"
        )))

    def test_checked_in_template_with_real_model_path(self):
        repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        real_template = os.path.join(
            repo_root, "config", "examples", "sze", "config_sze_mix153060_live.template.json"
        )
        real_model = os.path.join(repo_root, "sze", "model", "mix153060_sze_v04_a3_eff60.bin")
        if not (os.path.isfile(real_template) and os.path.isfile(real_model) and
                os.path.isfile(real_model + ".json")):
            self.skipTest("checked-in mix153060 model artifact is unavailable")

        with open(real_template) as handle:
            template = json.load(handle)
        codes = list(template["ins_params"].keys())
        dates = [self.target - datetime.timedelta(days=offset) for offset in range(21, 0, -1)]
        dates.append(self.target)
        for day_index, date in enumerate(dates):
            path = os.path.join(self.daily, "sze_daily_{}.csv".format(date.strftime("%Y%m%d")))
            with open(path, "w", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=[
                    "trade_dt", "s_info_windcode", "s_dq_close", "s_dq_preclose",
                    "s_dq_amount", "s_dq_limit", "s_dq_stopping",
                ])
                writer.writeheader()
                for code_index, code in enumerate(codes):
                    pre_close = 5.0 + float(code_index)
                    writer.writerow({
                        "trade_dt": date.strftime("%Y%m%d"),
                        "s_info_windcode": code,
                        "s_dq_close": "" if date == self.target else pre_close * math.exp(
                            0.0005 * float(day_index + code_index + 1)
                        ),
                        "s_dq_preclose": pre_close,
                        "s_dq_amount": "" if date == self.target else 100.0 + day_index,
                        "s_dq_limit": pre_close * 1.1,
                        "s_dq_stopping": pre_close * 0.9,
                    })

        free_share = os.path.join(self.root, "free_share_{}.csv".format(self.target.strftime("%Y%m%d")))
        with open(free_share, "w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=["date", "code", "free_share", "unit"])
            writer.writeheader()
            for code_index, code in enumerate(codes):
                writer.writerow({
                    "date": self.target.strftime("%Y%m%d"),
                    "code": code,
                    "free_share": 100000.0 + code_index,
                    "unit": "万股",
                })

        args = MODULE.parse_args([
            "--target-date", self.target.strftime("%Y%m%d"),
            "--template-json", real_template,
            "--daily-dir", self.daily,
            "--output-dir", self.output,
            "--md-source-index", "88",
            "--td-source-index", "180",
            "--free-share-file", free_share,
        ])
        result = MODULE.run(args)
        self.assertEqual(len(result["audit"]["instruments"]), len(codes))
        self.assertEqual(result["audit"]["model_sha256"], MODULE._sha256(real_model))
        self.assertEqual(result["config"]["md_source_index"], [88])
        self.assertEqual(result["config"]["td_source_index"], [180])
        for code in codes:
            self.assertGreater(result["config"]["ins_params"][code]["HistoryAmount"], 0.0)


if __name__ == "__main__":
    unittest.main()
