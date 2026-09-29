import copy
import importlib.util
import json
import os
import tempfile
import unittest


_MODULE_PATH = os.path.abspath(os.path.join(
    os.path.dirname(__file__), "../../tools/config/unified_config.py"
))
_SPEC = importlib.util.spec_from_file_location("unified_config", _MODULE_PATH)
unified_config = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(unified_config)

ConfigError = unified_config.ConfigError


class UnifiedConfigTests(unittest.TestCase):
    def setUp(self):
        self.sz_runtime = {
            "market": "SZ",
            "strategy_name": "test",
            "mode": "hp-realtime",
            "md_source_index": [88],
            "td_source_index": [180],
            "oms": {"simulation_cash": 1000000, "fee_reserve_per_order": 0},
            "model_path": "/fixtures/model.bin",
            "global_params": {"offset": 1.0, "position_limit": 1.0},
            "ins_params": {
                "000001": {
                    "Date": 20260904,
                    "Close": 10,
                    "HistoryAmount": 1000000,
                    "FreeShare": 10000000,
                    "HpUpperPrice": 11,
                    "HpLowerPrice": 9,
                    "HistoryVolatility20d": 0.02,
                    "static_position": 200,
                    "last_position": -100,
                }
            },
            "sze_order_routing": {"enabled": True, "mode": "live"},
        }
        self.sh_runtime = {
            "market": "SH",
            "strategy_name": "test",
            "runtime_mode": "live",
            "md_source_index": [89],
            "td_source_index": [190],
            "oms": {"simulation_cash": 1000000, "fee_reserve_per_order": 0},
            "trading_enabled": True,
            "prediction_only": False,
            "production_approval": False,
            "model_path": "/fixtures/model.bin",
            "model_type": "sse_hybrid_native",
            "snapshot_baseline_model_path": "/fixtures/model.snapshot",
            "snapshot_baseline_scaler_path": "/fixtures/scaler.snapshot",
            "snapshot_auction59_model_path": "/fixtures/model.backup",
            "snapshot_auction59_scaler_path": "/fixtures/scaler.backup",
            "global_params": {"offset": 1.0, "position_limit": 1.0},
            "ins_params": {
                "600000": {
                    "Date": 20260904,
                    "Close": 10,
                    "HistoryAmount": 1000000,
                    "FreeShare": 10000000,
                    "HpUpperPrice": 11,
                    "HpLowerPrice": 9,
                    "HistoryVolatility20d": 0.02,
                    "static_position": 200,
                    "last_position": -100,
                }
            },
            "sse_order_routing": {
                "enabled": True,
                "mode": "live",
                "td_source": 190,
            },
        }

    def migrate(self, runtime, day=20260904):
        return unified_config.migrate_runtime(runtime, "ACC-TEST", day=day)

    def test_sz_roundtrip_and_normalized_locations(self):
        config = self.migrate(self.sz_runtime)
        unified_config.validate(config)
        self.assertEqual("SZ", config["market"])
        self.assertEqual(self.sz_runtime, unified_config.export_legacy(config))
        self.assertEqual(self.sz_runtime["global_params"], config["strategy"]["parameters"])
        self.assertEqual("/fixtures/model.bin", config["prediction"]["model_path"])

    def test_external_delta_roundtrip_and_validation(self):
        for runtime, symbol in ((self.sz_runtime, "000001.SZ"), (self.sh_runtime, "600000.SH")):
            for delta in (100, -100, 0):
                modified = copy.deepcopy(runtime)
                next(iter(modified["ins_params"].values()))["external_delta"] = delta
                config = self.migrate(modified)
                unified_config.validate(config)
                self.assertEqual(delta, config["account"]["positions"][symbol]["external_delta"])
                self.assertEqual(modified, unified_config.export_legacy(config))
            for delta in (1.5, 2147483648, 300):
                modified = copy.deepcopy(runtime)
                next(iter(modified["ins_params"].values()))["external_delta"] = delta
                with self.assertRaises(ConfigError):
                    config = self.migrate(modified)
                    unified_config.validate(config)

    def test_sh_roundtrip_and_normalized_locations(self):
        config = self.migrate(self.sh_runtime)
        unified_config.validate(config)
        self.assertEqual("SH", config["market"])
        self.assertEqual(self.sh_runtime, unified_config.export_legacy(config))
        self.assertEqual("sse_hybrid_native", config["prediction"]["model_type"])
        self.assertEqual(
            self.sh_runtime["snapshot_baseline_model_path"],
            config["prediction"]["snapshot_baseline_model_path"],
        )
        self.assertEqual(
            self.sh_runtime["snapshot_baseline_scaler_path"],
            config["prediction"]["snapshot_baseline_scaler_path"],
        )
        self.assertEqual(
            self.sh_runtime["snapshot_auction59_model_path"],
            config["prediction"]["snapshot_auction59_model_path"],
        )
        self.assertEqual(
            self.sh_runtime["snapshot_auction59_scaler_path"],
            config["prediction"]["snapshot_auction59_scaler_path"],
        )

    def test_positions_are_separate_from_static_instruments(self):
        config = self.migrate(self.sz_runtime)
        instrument = config["daily"]["instruments"]["000001.SZ"]
        position = config["account"]["positions"]["000001.SZ"]
        self.assertEqual(20260904, instrument["Date"])
        self.assertNotIn("static_position", instrument)
        self.assertNotIn("last_position", instrument)
        self.assertEqual({"static_position": 200, "last_position": -100}, position)

    def test_unknown_top_level_field_is_rejected(self):
        config = self.migrate(self.sz_runtime)
        config["unexpected"] = True
        with self.assertRaises(ConfigError):
            unified_config.validate(config)

    def test_secrets_are_rejected_at_any_nesting_level(self):
        runtime = copy.deepcopy(self.sz_runtime)
        runtime["credentials"] = {"password": "secret"}
        with self.assertRaises(ConfigError):
            self.migrate(runtime)
        config = self.migrate(self.sz_runtime)
        config["strategy"]["parameters"]["trade_password"] = "secret"
        with self.assertRaises(ConfigError):
            unified_config.validate(config)

    def test_nonfinite_numbers_are_rejected(self):
        for value in (float("nan"), float("inf"), float("-inf")):
            runtime = copy.deepcopy(self.sz_runtime)
            runtime["global_params"]["offset"] = value
            with self.assertRaises(ConfigError):
                self.migrate(runtime)

    def test_invalid_date_is_rejected(self):
        runtime = copy.deepcopy(self.sz_runtime)
        runtime["ins_params"]["000001"]["Date"] = 20261399
        with self.assertRaises(ConfigError):
            self.migrate(runtime)

    def test_mixed_market_instruments_are_rejected(self):
        runtime = copy.deepcopy(self.sz_runtime)
        runtime["ins_params"]["600000"] = copy.deepcopy(runtime["ins_params"]["000001"])
        with self.assertRaises(ConfigError):
            self.migrate(runtime)

    def test_strings_and_bools_are_not_numeric_values(self):
        for value in ("1.0", True):
            runtime = copy.deepcopy(self.sz_runtime)
            runtime["global_params"]["offset"] = value
            with self.assertRaises(ConfigError):
                self.migrate(runtime)

    def test_negative_static_position_is_rejected_but_negative_last_is_valid(self):
        runtime = copy.deepcopy(self.sz_runtime)
        runtime["ins_params"]["000001"]["static_position"] = -1
        with self.assertRaises(ConfigError):
            self.migrate(runtime)
        config = self.migrate(self.sz_runtime)
        unified_config.validate(config)
        self.assertEqual(-100, config["account"]["positions"]["000001.SZ"]["last_position"])

    def test_environment_binding_and_processing_hash(self):
        config = self.migrate(self.sz_runtime)
        base_hash = unified_config.processing_hash(config)
        before = copy.deepcopy(config)
        live = unified_config.bind_environment(config, "live")
        self.assertEqual("disabled", live["environment"]["execution"])
        replay = unified_config.bind_environment(
            config,
            "replay",
            recording="/fixtures/day.raw",
            record_format="raw-datagrams",
            time_basis="recorded-receive",
        )
        self.assertEqual(base_hash, unified_config.processing_hash(live))
        self.assertEqual(base_hash, unified_config.processing_hash(replay))
        self.assertEqual(before, config)

    def test_replay_cannot_be_live_trading(self):
        config = self.migrate(self.sz_runtime)
        replay = unified_config.bind_environment(
            config, "replay", "/fixtures/day.csv", "csv", "exchange"
        )
        replay["environment"]["execution"] = "live"
        with self.assertRaises(ConfigError):
            unified_config.validate(replay)

    def test_replay_requires_valid_recording_arguments(self):
        config = self.migrate(self.sz_runtime)
        for args in ((None, "csv", "exchange"), ("/x", "json", "exchange"),
                     ("/x", "csv", "wallclock")):
            with self.assertRaises(ConfigError):
                unified_config.bind_environment(config, "replay", *args)

    def test_processing_hash_changes_for_strategy_or_model(self):
        config = self.migrate(self.sz_runtime)
        original = unified_config.processing_hash(config)
        changed_strategy = copy.deepcopy(config)
        changed_strategy["strategy"]["parameters"]["offset"] = 2.0
        changed_model = copy.deepcopy(config)
        changed_model["prediction"]["model_path"] = "/fixtures/other.bin"
        self.assertNotEqual(original, unified_config.processing_hash(changed_strategy))
        self.assertNotEqual(original, unified_config.processing_hash(changed_model))

    def test_duplicate_json_keys_are_rejected(self):
        fd, path = tempfile.mkstemp(suffix=".json")
        try:
            with os.fdopen(fd, "w") as stream:
                stream.write('{"market":"SZ", "market":"SH"}')
            with self.assertRaises(ConfigError):
                unified_config.load_json(path)
        finally:
            os.unlink(path)

    def test_exchange_time_csv_is_analysis_only_declaration(self):
        config = self.migrate(self.sz_runtime)
        replay = unified_config.bind_environment(
            config, "replay", "/fixtures/events.csv", "csv", "exchange"
        )
        self.assertEqual("exchange", replay["environment"]["time_basis"])
        self.assertEqual("disabled", replay["environment"]["execution"])
        unified_config.validate(replay)


if __name__ == "__main__":
    unittest.main()
