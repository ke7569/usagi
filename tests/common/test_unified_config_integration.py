import copy
import importlib.util
import os
import tempfile
import unittest


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
SPEC = importlib.util.spec_from_file_location(
    "unified_config", os.path.join(ROOT, "tools/config/unified_config.py"))
CONFIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONFIG)


def runtime():
    return {
        "market": "SH", "strategy_name": "test", "trading_day": 20260904,
        "model_path": "/fixtures/model.bin", "global_params": {"offset": 1.0},
        "md_source_index": [89], "td_source_index": [190],
        "sse_order_routing": {"mode": "live", "enabled": True, "td_source": 190},
        "ins_params": {"600000.SH": {
            "Date": 20260904, "Close": 10.0, "HistoryAmount": 1000000.0,
            "HistoryVolatility20d": 0.02, "FreeShare": 10000000.0,
            "HpLowerPrice": 9.0, "HpUpperPrice": 11.0, "static_position": 200,
        }},
    }


class UnifiedConfigIntegrationTest(unittest.TestCase):
    def test_sze_fixed_and_daily_preserve_generated_components(self):
        generator_path = os.path.join(ROOT, "deploy/sze/daily/prepare_sze_runtime.py")
        spec = importlib.util.spec_from_file_location("reference_generator", generator_path)
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        system_path = os.path.join(ROOT, "deploy/sze/daily/sze_system.json")
        daily_path = os.path.join(ROOT, "deploy/sze/daily/config_sze_daily_example.json")
        system, daily = CONFIG.load_json(system_path), CONFIG.load_json(daily_path)
        with tempfile.TemporaryDirectory(prefix="config-reference-") as directory:
            generator.strategy_configs(system, daily, daily["trading_day"], directory)
            manifest = CONFIG.load_json(os.path.join(directory, "manifest.json"))
            shard = next(item["shard"] for item in manifest["shards"] if item["symbol_count"])
            for component, relative in (("trade", "trade/config.json"),
                                        ("worker:" + str(shard), "workers/config_{:02d}.json".format(shard))):
                with self.subTest(component=component):
                    reference = CONFIG.load_json(os.path.join(directory, relative))
                    migrated = CONFIG.migrate_sze_system(
                        system_path, daily_path, "account-test", component)
                    self.assertEqual(reference, CONFIG.export_legacy(migrated))
                    self.assertEqual(CONFIG.canonical_hash(reference),
                                     migrated["migration"]["source_runtime_sha256"])

    def test_sse_daily_overlay_preserves_legacy_precedence_and_freezes_date(self):
        original = runtime()
        original["daily_config_path"] = "/daily/${TRADING_DATE}.json"
        daily = {"trading_day": 20260904, "static_data_source_date": 20260903,
                 "ins_params": copy.deepcopy(original["ins_params"]),
                 "global_params": {"offset": 2.0}}
        daily["static_data_hash"] = CONFIG.canonical_hash(daily["ins_params"])
        resolved = CONFIG.resolve_sse_daily(original, daily)
        self.assertNotIn("daily_config_path", resolved)
        self.assertIn("daily_config_path", original)
        self.assertEqual({"offset": 2.0}, resolved["global_params"])
        migrated = CONFIG.migrate_runtime(resolved, "account-test")
        self.assertEqual(resolved, CONFIG.export_legacy(migrated))
        for key, value in (("trading_day", 20260903), ("static_data_hash", "0" * 64),
                           ("unrecognized", 1)):
            bad = copy.deepcopy(daily)
            bad[key] = value
            with self.subTest(key=key), self.assertRaises(CONFIG.ConfigError):
                CONFIG.resolve_sse_daily(original, bad)

    def test_same_aliases_roundtrip_and_conflicts_are_rejected(self):
        original = runtime()
        original.update({"mode": "complete-orderbook-sh", "orderbook_mode": "complete-orderbook-sh"})
        config = CONFIG.migrate_runtime(original, "account-test")
        self.assertEqual(original, CONFIG.export_legacy(config))
        original["orderbook_mode"] = "different"
        with self.assertRaises(CONFIG.ConfigError):
            CONFIG.migrate_runtime(original, "account-test")

    def test_conflicting_redundant_position_arrays_are_rejected(self):
        original = runtime()
        original.update({"instrument_id": ["600000"], "static_position": [200]})
        CONFIG.migrate_runtime(original, "account-test")
        original["static_position"] = [300]
        with self.assertRaises(CONFIG.ConfigError):
            CONFIG.migrate_runtime(original, "account-test")

    def test_missing_parameters_are_not_replaced_with_new_defaults(self):
        original = runtime()
        del original["global_params"]
        config = CONFIG.migrate_runtime(original, "account-test")
        self.assertNotIn("parameters", config["strategy"])
        self.assertEqual(original, CONFIG.export_legacy(config))
        config["strategy"]["parameters"] = {"offset": 3.0}
        with self.assertRaises(CONFIG.ConfigError):
            CONFIG.validate(config)

    def test_nested_controls_require_correct_types(self):
        for field, value in (("enabled", "false"), ("max_position", True),
                             ("mode", "typo"), ("td_source", 180)):
            original = runtime()
            original["sse_order_routing"][field] = value
            with self.subTest(field=field), self.assertRaises(CONFIG.ConfigError):
                CONFIG.migrate_runtime(original, "account-test")

    def test_invalid_modes_and_missing_live_source_fail(self):
        for field, value in (("runtime_mode", "typo"), ("mode", False),
                             ("strategy_name", 3), ("td_source_index", [])):
            original = runtime()
            original[field] = value
            with self.subTest(field=field), self.assertRaises(CONFIG.ConfigError):
                CONFIG.migrate_runtime(original, "account-test")

    def test_lock_detects_artifact_and_parameter_drift_across_modes(self):
        with tempfile.TemporaryDirectory(prefix="config-artifacts-") as directory:
            model = os.path.join(directory, "model.bin")
            binary = os.path.join(directory, "strategy.so")
            for path, content in ((model, b"model-v1"), (binary, b"runtime-v1")):
                with open(path, "wb") as stream:
                    stream.write(content)
            original = runtime()
            original["model_path"] = model
            config = CONFIG.migrate_runtime(original, "account-test")
            lock = CONFIG.make_lock(config, [binary])
            replay = CONFIG.bind_environment(config, "replay", "/recordings/day.bin",
                                              "raw-datagrams", "recorded-receive")
            live = CONFIG.bind_environment(config, "live")
            self.assertTrue(CONFIG.verify_lock(replay, lock))
            self.assertTrue(CONFIG.verify_lock(live, lock))
            altered = copy.deepcopy(config)
            altered["strategy"]["parameters"]["offset"] = 2.0
            with self.assertRaises(CONFIG.ConfigError):
                CONFIG.verify_lock(altered, lock)
            for path in (model, binary):
                with self.subTest(path=path):
                    with open(path, "rb") as stream:
                        old = stream.read()
                    with open(path, "wb") as stream:
                        stream.write(b"changed")
                    with self.assertRaises(CONFIG.ConfigError):
                        CONFIG.verify_lock(config, lock)
                    with open(path, "wb") as stream:
                        stream.write(old)
            with self.assertRaises(CONFIG.ConfigError):
                CONFIG.make_lock(config, [])

    def test_run_manifest_detects_recording_and_clock_changes(self):
        with tempfile.TemporaryDirectory(prefix="config-run-") as directory:
            paths = [os.path.join(directory, name) for name in ("model", "binary", "recording")]
            for path in paths:
                with open(path, "wb") as stream:
                    stream.write(b"original")
            original = runtime()
            original["model_path"] = paths[0]
            config = CONFIG.bind_environment(
                CONFIG.migrate_runtime(original, "account-test"), "replay",
                paths[2], "canonical-events", "recorded-receive")
            artifacts = CONFIG.make_lock(config, [paths[1]])
            manifest = CONFIG.make_run_manifest(config, artifacts)
            self.assertTrue(CONFIG.verify_run_manifest(config, artifacts, manifest))
            changed = copy.deepcopy(config)
            changed["environment"]["time_basis"] = "exchange"
            self.assertEqual(CONFIG.processing_hash(config), CONFIG.processing_hash(changed))
            with self.assertRaises(CONFIG.ConfigError):
                CONFIG.verify_run_manifest(changed, artifacts, manifest)
            with open(paths[2], "wb") as stream:
                stream.write(b"different-recording")
            with self.assertRaises(CONFIG.ConfigError):
                CONFIG.verify_run_manifest(config, artifacts, manifest)
            changed["environment"]["recording"] = directory
            with self.assertRaises(CONFIG.ConfigError):
                CONFIG.make_run_manifest(changed, artifacts)

    def test_missing_artifacts_are_not_treated_as_verified(self):
        config = CONFIG.migrate_runtime(runtime(), "account-test")
        with self.assertRaises(CONFIG.ConfigError):
            CONFIG.make_lock(config, ["/does-not-exist/strategy.so"])

    def test_segmented_recording_manifest_detects_changes_and_holes(self):
        with tempfile.TemporaryDirectory(prefix="config-segments-") as directory:
            model = os.path.join(directory, "model.bin")
            binary = os.path.join(directory, "runtime")
            recording = os.path.join(directory, "recording")
            os.mkdir(recording)
            for path in (model, binary, os.path.join(recording, "stream_000000.t0md")):
                with open(path, "wb") as stream:
                    stream.write(b"manifest-input")
            original = runtime()
            original["model_path"] = model
            config = CONFIG.bind_environment(CONFIG.migrate_runtime(original, "account-test"),
                "replay", recording, "t0md-v1", "recorded-receive")
            lock = CONFIG.make_lock(config, [binary])
            manifest = CONFIG.make_run_manifest(config, lock)
            self.assertTrue(CONFIG.verify_run_manifest(config, lock, manifest))
            with open(os.path.join(recording, "stream_000000.t0md"), "wb") as stream:
                stream.write(b"changed")
            with self.assertRaises(CONFIG.ConfigError):
                CONFIG.verify_run_manifest(config, lock, manifest)
            with open(os.path.join(recording, "stream_000002.t0md"), "wb") as stream:
                stream.write(b"gap")
            with self.assertRaises(CONFIG.ConfigError):
                CONFIG.make_run_manifest(config, lock)
            with self.assertRaises(CONFIG.ConfigError):
                CONFIG.bind_environment(config, "replay", recording, "t0md-v1", "exchange")

    def test_config_file_nonfinite_and_duplicate_keys_fail(self):
        with tempfile.TemporaryDirectory(prefix="config-json-") as directory:
            path = os.path.join(directory, "bad.json")
            for text in ('{"offset":NaN}', '{"nested":{"x":1,"x":2}}'):
                with open(path, "w") as stream:
                    stream.write(text)
                with self.subTest(text=text), self.assertRaises(CONFIG.ConfigError):
                    CONFIG.load_json(path)


if __name__ == "__main__":
    unittest.main()
