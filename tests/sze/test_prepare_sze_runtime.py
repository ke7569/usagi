#!/usr/bin/env python3

import copy
import hashlib
import importlib.util
import json
import os
import shutil
import tempfile
import unittest


HERE = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../deploy/sze/daily"))
SPEC = importlib.util.spec_from_file_location(
    "prepare_sze_runtime", os.path.join(HERE, "prepare_sze_runtime.py"))
RUNTIME = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNTIME)
TD_SPEC = importlib.util.spec_from_file_location(
    "merge_sze_td_runtime", os.path.join(HERE, "merge_sze_td_runtime.py"))
TD_RUNTIME = importlib.util.module_from_spec(TD_SPEC)
TD_SPEC.loader.exec_module(TD_RUNTIME)
MIGRATE_SPEC = importlib.util.spec_from_file_location(
    "migrate_legacy_daily", os.path.join(HERE, "migrate_legacy_daily.py"))
MIGRATE = importlib.util.module_from_spec(MIGRATE_SPEC)
MIGRATE_SPEC.loader.exec_module(MIGRATE)


class RuntimeConfigTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.mkdtemp(prefix="sze-config-test-")
        self.system = RUNTIME.load_json(os.path.join(HERE, "sze_system.json"))
        self.daily = RUNTIME.load_json(
            os.path.join(HERE, "config_sze_daily_example.json"))
        self.model = os.path.join(self.temp, "model.bin")
        with open(self.model, "wb") as stream:
            stream.write(b"test-model")
        self.system["model"]["path"] = self.model
        self.system["model"]["sha256"] = hashlib.sha256(b"test-model").hexdigest()
        self.system["paths"]["runtime_root"] = os.path.join(self.temp, "run")
        self.system["paths"]["journal_directory_pattern"] = os.path.join(
            self.temp, "journal_{trading_day}")
        self.system["paths"]["shm_path_pattern"] = os.path.join(
            self.temp, "sze_{trading_day}.events")
        credentials = os.path.join(self.temp, "td_credentials.json")
        with open(credentials, "w") as stream:
            json.dump({"td": {"password": "SECRET_MUST_NOT_LEAK"}}, stream)
        os.chmod(credentials, 0o600)
        self.system["trade"]["credentials_path"] = credentials

    def tearDown(self):
        shutil.rmtree(self.temp)

    def test_capture_has_no_daily_dependency(self):
        RUNTIME.validate_system(self.system, True)
        output = os.path.join(self.temp, "capture")
        RUNTIME.capture_configs(self.system, 20260817, output)
        deepwin = RUNTIME.load_json(os.path.join(output, "deepwin.json"))
        pipeline = deepwin["md"]["sze"]["recoverable_pipeline"]
        self.assertEqual(1024, pipeline["journal_segment_mb"])
        self.assertEqual(80, pipeline["journal_min_free_gb_after_allocate"])
        self.assertEqual(20260817, pipeline["trading_day"])

    def test_valid_daily_generates_stable_shards_and_trade(self):
        daily = RUNTIME.validate_daily(copy.deepcopy(self.daily), 20260817)
        output = os.path.join(self.temp, "strategy")
        RUNTIME.strategy_configs(self.system, daily, 20260817, output)
        manifest = RUNTIME.load_json(os.path.join(output, "manifest.json"))
        self.assertEqual(8, len(manifest["shards"]))
        self.assertEqual(["000001.SZ"], manifest["trade_symbols"])
        self.assertTrue(os.path.isfile(os.path.join(output, "trade", "main.conf")))
        worker = RUNTIME.load_json(os.path.join(output, "workers", "config_00.json"))
        trade = RUNTIME.load_json(os.path.join(output, "trade", "config.json"))
        self.assertEqual("hp-shadow", worker["mode"])
        self.assertTrue(worker["sze_prediction_capture"]["capture_only"])
        self.assertEqual("hp-realtime", trade["mode"])
        self.assertTrue(trade["sze_recovery_consumer"]["trading_enabled"])
        all_text = ""
        for root, _, files in os.walk(output):
            for name in files:
                with open(os.path.join(root, name)) as stream:
                    all_text += stream.read()
        self.assertNotIn("SECRET_MUST_NOT_LEAK", all_text)

    def test_stale_day_is_rejected(self):
        with self.assertRaises(RUNTIME.ConfigError):
            RUNTIME.validate_daily(copy.deepcopy(self.daily), 20260818)

    def test_static_hash_mismatch_is_rejected(self):
        daily = copy.deepcopy(self.daily)
        daily["ins_params"]["000001.SZ"]["static_position"] = 0
        with self.assertRaises(RUNTIME.ConfigError):
            RUNTIME.validate_daily(daily, 20260817)

    def test_invalid_symbol_is_rejected(self):
        daily = copy.deepcopy(self.daily)
        daily["ins_params"]["000001"] = daily["ins_params"].pop("000001.SZ")
        daily["static_data_hash"] = RUNTIME.canonical_hash(daily["ins_params"])
        with self.assertRaises(RUNTIME.ConfigError):
            RUNTIME.validate_daily(daily, 20260817)

    def test_daily_cpu_and_last_position_are_rejected(self):
        for field in ("cpu", "last_position"):
            daily = copy.deepcopy(self.daily)
            daily["ins_params"]["000001.SZ"][field] = 1
            daily["static_data_hash"] = RUNTIME.canonical_hash(daily["ins_params"])
            with self.assertRaises(RUNTIME.ConfigError):
                RUNTIME.validate_daily(daily, 20260817)

    def test_duplicate_json_key_is_rejected(self):
        path = os.path.join(self.temp, "duplicate.json")
        with open(path, "w") as stream:
            stream.write('{"trading_day":1,"trading_day":2}')
        with self.assertRaises(RUNTIME.ConfigError):
            RUNTIME.load_json(path)

    def test_legacy_daily_is_rejected(self):
        legacy = copy.deepcopy(self.daily)
        legacy["worker_count"] = 8
        legacy["ins_params"]["000001.SZ"]["cpu"] = 16
        legacy["ins_params"]["000001.SZ"]["last_position"] = 0
        with self.assertRaises(RUNTIME.ConfigError):
            RUNTIME.validate_daily(copy.deepcopy(legacy), 20260817)
        with self.assertRaises(RUNTIME.ConfigError):
            RUNTIME.validate_daily(legacy, 20260817)

    def test_legacy_migration_produces_strict_daily_hash(self):
        legacy = copy.deepcopy(self.daily)
        legacy["worker_count"] = 8
        legacy["ins_params"]["000001.SZ"]["cpu"] = 16
        legacy["ins_params"]["000001.SZ"]["last_position"] = 0
        converted = MIGRATE.convert(legacy, 20260817, 20260816)
        self.assertEqual(set(converted), RUNTIME.DAILY_KEYS)
        self.assertEqual(
            converted["static_data_hash"],
            RUNTIME.canonical_hash(converted["ins_params"]))
        self.assertNotIn("cpu", converted["ins_params"]["000001.SZ"])
        self.assertNotIn("last_position", converted["ins_params"]["000001.SZ"])
        RUNTIME.validate_daily(converted, 20260817)

    def test_credentials_permissions_are_enforced(self):
        path = self.system["trade"]["credentials_path"]
        os.chmod(path, 0o644)
        with self.assertRaises(RUNTIME.ConfigError):
            RUNTIME.validate_credentials(self.system)

    def test_bad_credentials_do_not_block_prediction_validation(self):
        os.chmod(self.system["trade"]["credentials_path"], 0o644)
        RUNTIME.validate_system(self.system, True)
        RUNTIME.validate_daily(copy.deepcopy(self.daily), 20260817)

    def test_td_cpu_plan_overrides_private_values(self):
        base = os.path.join(self.temp, "deepwin.json")
        private = self.system["trade"]["credentials_path"]
        system = os.path.join(self.temp, "system.json")
        output = os.path.join(self.temp, "deepwin.private.json")
        with open(base, "w") as stream:
            json.dump({"md": {"sze": {}}}, stream)
        with open(private, "w") as stream:
            json.dump({"td": {"sze_td": {
                "password": "SECRET_MUST_NOT_LEAK_TO_LOGS",
                "receive_thread_cpu": 0,
                "send_thread_cpu": 0,
                "accounts": [{"info": {"receive_thread_cpu": 1,
                                          "send_thread_cpu": 1}}],
            }}}, stream)
        os.chmod(private, 0o600)
        with open(system, "w") as stream:
            json.dump(self.system, stream)
        TD_RUNTIME.merge(base, private, system, output)
        merged = RUNTIME.load_json(output)["td"]["sze_td"]
        self.assertEqual(36, merged["receive_thread_cpu"])
        self.assertEqual(37, merged["send_thread_cpu"])
        self.assertEqual(36, merged["accounts"][0]["info"]["receive_thread_cpu"])
        self.assertEqual(0o600, os.stat(output).st_mode & 0o777)


if __name__ == "__main__":
    unittest.main()
