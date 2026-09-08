import copy
import os
import sys
import tempfile
import unittest


_TOOLS = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../tools/config"))
sys.path.insert(0, _TOOLS)
import prepare_stream_processing as processing
import unified_config


class PrepareStreamProcessingTests(unittest.TestCase):
    def runtime(self, market="SZ"):
        symbol = "000001.SZ" if market == "SZ" else "600000.SH"
        result = {
            "market": market,
            "trading_day": 20260904,
            "oms": {"simulation_cash": 1000000, "fee_reserve_per_order": 0},
            "model_path": "/absent-fixtures/tick.bin",
            "global_params": {"position_limit": 0.75, "offset": 1.25},
            "ins_params": {symbol: {
                "Date": 20260904,
                "Close": 10.0,
                "HistoryAmount": 8000000.0,
                "FreeShare": 10000000.0,
                "HpUpperPrice": 11.0,
                "HpLowerPrice": 9.0,
                "HistoryVolatility20d": 0.02,
                "static_position": 200,
                "last_position": -100,
            }},
        }
        if market == "SZ":
            result["sz_orderbook_mode"] = "hp-realtime"
        else:
            result["sh_orderbook_mode"] = "complete-orderbook-sh"
            result["model_type"] = "sse_hybrid_native"
            result.update({
                "snapshot_baseline_model_path": "/absent-fixtures/baseline.bin",
                "snapshot_baseline_scaler_path": "/absent-fixtures/baseline.json",
                "snapshot_auction59_model_path": "/absent-fixtures/auction59.bin",
                "snapshot_auction59_scaler_path": "/absent-fixtures/auction59.json",
                "snapshot_auction59_factors_path": "/absent-fixtures/auction59.csv",
            })
        return result

    def config(self, market="SZ", runtime=None):
        source = runtime if runtime is not None else self.runtime(market)
        config = unified_config.migrate_runtime(source, "ACCOUNT-TEST")
        return unified_config.bind_environment(config, "live")

    def test_sz_profile_has_exact_nontrading_schema_and_mapping(self):
        config = self.config()
        result = processing.make_profile(config)
        self.assertEqual({
            "schema_version", "market", "execution", "processing_mode", "trading_day",
            "processing_sha256", "environment", "prediction", "instruments", "processing_contract",
        }, set(result))
        self.assertEqual(1, result["schema_version"])
        self.assertEqual("SZ", result["market"])
        self.assertEqual("prediction", result["processing_mode"])
        self.assertEqual("sze-mix153060-v04", result["processing_contract"])
        self.assertEqual("disabled", result["execution"])
        self.assertEqual(20260904, result["trading_day"])
        self.assertEqual(unified_config.processing_hash(config), result["processing_sha256"])
        self.assertEqual([{
            "instrument": "000001", "trading_date": 20260904,
            "average_amount": 8000000.0, "turnover_threshold": 1000.0,
            "free_share": 10000000.0, "pre_close": 10.0,
            "upper_limit": 11.0, "lower_limit": 9.0,
            "history_volatility_20d": 0.02,
        }], result["instruments"])
        self.assertNotIn("account", result)
        self.assertNotIn("strategy", result)

    def test_sh_profile_preserves_native_model_paths_and_static_mapping(self):
        config = self.config("SH")
        result = processing.make_profile(config)
        self.assertEqual("SH", result["market"])
        self.assertEqual("sse-per-instrument-v2", result["processing_contract"])
        self.assertEqual("600000", result["instruments"][0]["instrument"])
        self.assertEqual(1000.0, result["instruments"][0]["turnover_threshold"])
        self.assertEqual(11.0, result["instruments"][0]["upper_limit"])
        self.assertEqual(config["prediction"], result["prediction"])
        self.assertEqual(config["environment"], result["environment"])

    def test_factors_only_is_explicit_and_keeps_model_path_requirements(self):
        config = self.config()
        result = processing.make_profile(config, factors_only=True)
        self.assertEqual("factors-only", result["processing_mode"])
        self.assertEqual(config["prediction"], result["prediction"])
        for value in ("false", 0, 1, None):
            with self.subTest(value=value), self.assertRaises(unified_config.ConfigError):
                processing.make_profile(config, factors_only=value)
        del config["prediction"]["model_path"]
        for mode in (False, True):
            with self.subTest(factors_only=mode), self.assertRaises(unified_config.ConfigError):
                processing.make_profile(config, factors_only=mode)

    def test_sh_prediction_requires_auction_inputs_but_factors_only_does_not(self):
        runtime = self.runtime("SH")
        del runtime["snapshot_auction59_factors_path"]
        config = self.config("SH", runtime=runtime)
        with self.assertRaises(unified_config.ConfigError):
            processing.make_profile(config)
        self.assertEqual("factors-only", processing.make_profile(config, factors_only=True)["processing_mode"])

    def test_strategy_intents_export_original_configuration_without_changing_base_profile(self):
        runtime = self.runtime("SH")
        runtime["td_source_index"] = [190]
        runtime["sse_order_routing"] = {"enabled": True, "mode": "live", "td_source": 190}
        runtime["ins_params"]["600000.SH"]["vol_unit"] = 100
        config = self.config("SH", runtime)
        original = copy.deepcopy(config)
        base_profile = processing.make_profile(config)
        self.assertNotIn("strategy_runtime", base_profile)
        self.assertEqual(base_profile, processing.make_profile(config, strategy_intents=False))
        profile = processing.make_profile(config, strategy_intents=True)
        self.assertEqual(set(base_profile) | {"strategy_runtime"}, set(profile))
        self.assertEqual(base_profile, {key: value for key, value in profile.items()
                                      if key != "strategy_runtime"})
        self.assertEqual({"mode": "paper-intents", "account_reference": "ACCOUNT-TEST",
                          "oms": {"simulation_cash": 1000000, "fee_reserve_per_order": 0},
                          "legacy_config": runtime},
                         profile["strategy_runtime"])
        self.assertEqual("disabled", profile["execution"])
        self.assertEqual("disabled", profile["environment"]["execution"])
        self.assertEqual(unified_config.processing_hash(config), profile["processing_sha256"])
        self.assertEqual(original, config)
        profile["strategy_runtime"]["legacy_config"]["global_params"]["offset"] = 9
        profile["strategy_runtime"]["legacy_config"]["ins_params"]["600000.SH"]["static_position"] = 999
        self.assertEqual(original, config)
        self.assertEqual(runtime, unified_config.export_legacy(config))

    def test_strategy_intents_reject_unsupported_markets_modes_and_nonboolean_values(self):
        self.assertEqual("paper-intents",
                         processing.make_profile(self.config("SZ"), strategy_intents=True)
                         ["strategy_runtime"]["mode"])
        for market, factors_only in (("SZ", True), ("SH", True)):
            config = self.config(market)
            with self.subTest(market=market, factors_only=factors_only):
                with self.assertRaisesRegex(unified_config.ConfigError, "prediction mode"):
                    processing.make_profile(config, factors_only=factors_only, strategy_intents=True)
        for value in ("true", "false", 0, 1, None, []):
            with self.subTest(value=value):
                with self.assertRaisesRegex(unified_config.ConfigError, "strategy_intents must be boolean"):
                    processing.make_profile(self.config("SH"), strategy_intents=value)

    def test_strategy_intents_require_explicit_oms_budget(self):
        config = self.config("SH")
        del config["trading"]["oms"]
        with self.assertRaisesRegex(unified_config.ConfigError, "trading.oms"):
            processing.make_profile(config, strategy_intents=True)

    def test_oms_budget_rejects_unknown_missing_bad_precision_and_bounds(self):
        cases = (
            {"simulation_cash": 1000000, "fee_reserve_per_order": 0, "unknown": 1},
            {"simulation_cash": 1000000},
            {"simulation_cash": -1, "fee_reserve_per_order": 0},
            {"simulation_cash": float("nan"), "fee_reserve_per_order": 0},
            {"simulation_cash": 1.00001, "fee_reserve_per_order": 0},
            {"simulation_cash": 1000000000001, "fee_reserve_per_order": 0},
            {"simulation_cash": True, "fee_reserve_per_order": 0},
        )
        for oms in cases:
            config = self.config("SH")
            config["trading"]["oms"] = oms
            with self.subTest(oms=oms), self.assertRaises(unified_config.ConfigError):
                processing.make_profile(config, strategy_intents=True)

    def recovery(self, config, **overrides):
        value = {
            "enabled": True,
            "allow_invalid_replay_for_analysis": False,
            "trading_enabled": False,
            "trading_day": 20260904,
            "source_id": 88,
            "journal_directory": "/tmp/sze-journal",
            "journal_prefix": "sze_all",
            "journal_segment_mb": 1024,
            "journal_max_payload_bytes": 128,
            "shm_path": "/tmp/sze.events",
        }
        value.update(overrides)
        result = copy.deepcopy(config)
        result["market_data"]["recovery"] = value
        result["migration"]["top_fields"].append("sze_recovery_consumer")
        return result

    def test_sz_recovery_journal_projects_existing_recovery_fields(self):
        config = self.recovery(self.config())
        config = unified_config.bind_environment(
            config, "replay", "/tmp/sze-journal", "canonical-events", "exchange")
        profile = processing.make_profile(config, recovery_input="journal",
                                          factors_only=True)
        self.assertEqual("sze-journal", profile["input_driver"])
        self.assertEqual(config["market_data"]["recovery"], profile["recovery"])
        self.assertEqual("disabled", profile["execution"])

    def test_sz_recovery_handoff_requires_live_epoch_and_rejects_analysis_flag(self):
        config = self.recovery(self.config(), expected_generation=17)
        profile = processing.make_profile(config, recovery_input="handoff")
        self.assertEqual("sze-handoff", profile["input_driver"])
        self.assertEqual(17, profile["recovery"]["expected_generation"])
        self.assertEqual("disabled", profile["environment"]["execution"])
        for bad in (
                self.recovery(self.config(), expected_generation=0),
                self.recovery(self.config(), expected_generation=17,
                              allow_invalid_replay_for_analysis=True),
        ):
            with self.subTest(bad=bad["market_data"]["recovery"]):
                with self.assertRaises(unified_config.ConfigError):
                    processing.make_profile(bad, recovery_input="handoff")

    def test_recovery_projection_rejects_market_environment_and_unknown_options(self):
        with self.assertRaisesRegex(unified_config.ConfigError, "only supported for SZ"):
            processing.make_profile(self.recovery(self.config("SH")), recovery_input="journal")
        with self.assertRaisesRegex(unified_config.ConfigError, "unknown recovery option"):
            processing.make_profile(self.recovery(self.config(), unexpected=1),
                                    recovery_input="journal")
        config = self.recovery(self.config())
        with self.assertRaisesRegex(unified_config.ConfigError, "canonical-events"):
            processing.make_profile(config, recovery_input="journal")

    def test_recovery_rejects_bool_bounds_mixed_segments_and_path_mismatch(self):
        cases = (
            {"source_id": True},
            {"source_id": 65536},
            {"trading_day": True},
            {"journal_max_payload_bytes": 65536},
            {"journal_max_payload_bytes": 71},
            {"journal_segment_mb": 1, "journal_segment_bytes": 1048576},
            {"journal_segment_bytes": 4096 + 72 + 16 + 128 - 1},
            {"expected_generation": 0},
            {"expected_generation": True},
            {"allow_invalid_replay_for_analysis": True},
        )
        for change in cases:
            config = self.recovery(self.config(), **change)
            with self.subTest(change=change), self.assertRaises(unified_config.ConfigError):
                config = unified_config.bind_environment(
                    config, "replay", "/tmp/sze-journal", "canonical-events", "exchange")
                processing.make_profile(config, recovery_input="journal")
        config = self.recovery(self.config())
        config = unified_config.bind_environment(
            config, "replay", "/tmp/other-journal", "canonical-events", "exchange")
        with self.assertRaisesRegex(unified_config.ConfigError, "match"):
            processing.make_profile(config, recovery_input="journal")

    def test_strategy_intents_keep_disabled_execution_requirement_for_live_and_replay(self):
        live = self.config("SH")
        replay = unified_config.bind_environment(
            live, "replay", "/absent-fixtures/recording", "t0md-v1", "recorded-receive")
        for config in (live, replay):
            self.assertEqual("paper-intents",
                             processing.make_profile(config, strategy_intents=True)["strategy_runtime"]["mode"])
            for execution in ("paper", "live"):
                changed = copy.deepcopy(config)
                changed["environment"]["execution"] = execution
                with self.subTest(mode=config["environment"]["mode"], execution=execution):
                    with self.assertRaises(unified_config.ConfigError):
                        processing.make_profile(changed, strategy_intents=True)

    def test_missing_static_fields_remain_rejected_by_unified_schema(self):
        for market in ("SZ", "SH"):
            for field in ("Close", "HistoryAmount", "FreeShare", "HpUpperPrice",
                          "HpLowerPrice", "HistoryVolatility20d"):
                config = self.config(market)
                values = next(iter(config["daily"]["instruments"].values()))
                del values[field]
                with self.subTest(market=market, field=field), self.assertRaises(unified_config.ConfigError):
                    processing.make_profile(config, factors_only=True)

    def test_nonpositive_static_metadata_is_rejected(self):
        for field in ("Close", "HistoryAmount", "FreeShare", "HpUpperPrice", "HpLowerPrice"):
            for value in (0, -1):
                config = self.config()
                config["daily"]["instruments"]["000001.SZ"][field] = value
                with self.subTest(field=field, value=value), self.assertRaises(unified_config.ConfigError):
                    processing.make_profile(config)

    def test_free_share_alias_and_zero_volatility_are_preserved(self):
        runtime = self.runtime()
        values = runtime["ins_params"]["000001.SZ"]
        values["free_share"] = values.pop("FreeShare")
        values["HistoryVolatility20d"] = 0.0
        result = processing.make_profile(self.config(runtime=runtime))
        self.assertEqual(10000000.0, result["instruments"][0]["free_share"])
        self.assertEqual(0.0, result["instruments"][0]["history_volatility_20d"])

    def test_per_instrument_limits_take_precedence_over_global_values(self):
        runtime = self.runtime()
        runtime["hp_upper_price"] = 12.0
        runtime["hp_lower_price"] = 8.0
        result = processing.make_profile(self.config(runtime=runtime))
        self.assertEqual(11.0, result["instruments"][0]["upper_limit"])
        self.assertEqual(9.0, result["instruments"][0]["lower_limit"])
        self.assertEqual(12.0, result["prediction"]["hp_upper_price"])

    def test_sh_requires_all_model_paths_even_without_explicit_model_type(self):
        for field in ("model_path", "snapshot_baseline_model_path",
                      "snapshot_baseline_scaler_path", "snapshot_auction59_model_path",
                      "snapshot_auction59_scaler_path"):
            config = self.config("SH")
            del config["prediction"]["model_type"]
            config["migration"]["top_fields"].remove("model_type")
            del config["prediction"][field]
            config["migration"]["top_fields"].remove(field)
            for mode in (False, True):
                with self.subTest(field=field, factors_only=mode), self.assertRaises(unified_config.ConfigError):
                    processing.make_profile(config, factors_only=mode)

    def test_sh_accepts_existing_sampling_and_routing_contract_declarations(self):
        canonical = unified_config.load_json(os.path.join(
            _TOOLS, "..", "..", "config", "examples", "sse",
            "config_sse_hybrid_prediction_20260818.json"))
        runtime = self.runtime("SH")
        runtime["sse_live_sampling"] = canonical["sse_live_sampling"]
        runtime["model_routing"] = canonical["model_routing"]
        self.assertEqual("global-sse-datagram-gap",
                         runtime["sse_live_sampling"]["activity_scope"])
        self.assertEqual("at-most-one-sample",
                         runtime["sse_live_sampling"]["same_exchange_time_policy"])
        self.assertEqual("first-valid-book-at-or-after-open",
                         runtime["sse_live_sampling"]["initial_window"])
        config = self.config("SH", runtime)
        result = processing.make_profile(config)
        self.assertEqual(config["prediction"], result["prediction"])
        runtime["sse_live_sampling"] = {"threshold_ns": 100000}
        runtime["model_routing"] = {"silent_fallback": False}
        result = processing.make_profile(self.config("SH", runtime))
        self.assertEqual("sse-per-instrument-v2", result["processing_contract"])

    def test_sh_rejects_global_scope_repeated_exchange_time_and_preopen_window(self):
        for declaration in (
                {"activity_scope": "per-instrument-sse-book-update"},
                {"same_exchange_time_policy": "allow-repeated-samples"},
                {"initial_window": "09:25:00"}):
            runtime = self.runtime("SH")
            runtime["sse_live_sampling"] = declaration
            config = self.config("SH", runtime)
            for factors_only in (False, True):
                with self.subTest(declaration=declaration, factors_only=factors_only):
                    with self.assertRaises(unified_config.ConfigError):
                        processing.make_profile(config, factors_only=factors_only)

    def test_sh_rejects_conflicting_sampling_fields_and_unknown_nested_fields(self):
        declarations = (
            {"threshold_ns": 200000},
            {"clock": "CLOCK_REALTIME"},
            {"comparison": "strict-greater-than"},
            {"shutdown_flush": True},
            {"periodic_md": True},
            {"unknown_sampling": 1},
            {"standard_gate": {"exchange_time_trigger_us": 100000}},
            {"standard_gate": {"mid_change_epsilon": 0.1}},
            {"standard_gate": {"min_volume_change": True}},
            {"standard_gate": {"unknown_gate": 1}},
        )
        for declaration in declarations:
            runtime = self.runtime("SH")
            runtime["sse_live_sampling"] = declaration
            with self.subTest(declaration=declaration), self.assertRaises(unified_config.ConfigError):
                processing.make_profile(self.config("SH", runtime))

    def test_sh_rejects_conflicting_routing_windows_clocks_and_unknown_fields(self):
        declarations = (
            {"clock": "CLOCK_MONOTONIC"},
            {"snapshot_selected_window": "[09:30:00,09:41:00)"},
            {"tick_selected_window": "[09:41:00,24:00:00)"},
            {"tick_warm_before_switch": False},
            {"silent_fallback": True},
            {"snapshot_generation_window": "[09:30:00,09:41:00)"},
        )
        for declaration in declarations:
            runtime = self.runtime("SH")
            runtime["model_routing"] = declaration
            with self.subTest(declaration=declaration), self.assertRaises(unified_config.ConfigError):
                processing.make_profile(self.config("SH", runtime), factors_only=True)

    def test_unsupported_market_book_and_sh_model_modes_are_rejected(self):
        config = self.config()
        config["market"] = "BJ"
        with self.assertRaises(unified_config.ConfigError):
            processing.make_profile(config)
        for market, wrong_mode in (("SZ", "full-orderbook"), ("SH", "hp-realtime")):
            config = self.config(market)
            config["prediction"]["book_mode"] = wrong_mode
            with self.subTest(market=market), self.assertRaises(unified_config.ConfigError):
                processing.make_profile(config)
        config = self.config("SH")
        config["prediction"]["model_type"] = "real_gru"
        with self.assertRaises(unified_config.ConfigError):
            processing.make_profile(config)

    def test_bound_environment_and_disabled_execution_are_required(self):
        config = unified_config.migrate_runtime(self.runtime(), "ACCOUNT-TEST")
        with self.assertRaises(unified_config.ConfigError):
            processing.make_profile(config)
        for execution in ("paper", "live"):
            config = self.config()
            config["environment"]["execution"] = execution
            with self.subTest(execution=execution), self.assertRaises(unified_config.ConfigError):
                processing.make_profile(config)

    def test_replay_accepts_only_t0md_recorded_receive_environment(self):
        config = self.config()
        replay = unified_config.bind_environment(
            config, "replay", "/absent-fixtures/recording", "t0md-v1", "recorded-receive")
        result = processing.make_profile(replay)
        self.assertEqual(replay["environment"], result["environment"])
        self.assertEqual(unified_config.processing_hash(config), result["processing_sha256"])
        for record_format, time_basis in (("raw-datagrams", "recorded-receive"),
                                           ("canonical-events", "recorded-receive"),
                                           ("csv", "exchange"), ("t0md-v1", "exchange")):
            changed = copy.deepcopy(replay)
            changed["environment"].update(record_format=record_format, time_basis=time_basis)
            with self.subTest(record_format=record_format, time_basis=time_basis), self.assertRaises(unified_config.ConfigError):
                processing.make_profile(changed)

    def test_input_is_unchanged_and_profile_does_not_alias_original(self):
        config = self.config("SH")
        before = copy.deepcopy(config)
        result = processing.make_profile(config, factors_only=True)
        self.assertEqual(before, config)
        result["prediction"]["model_path"] = "/changed"
        result["environment"]["mode"] = "changed"
        result["instruments"][0]["pre_close"] = 20.0
        self.assertEqual(before, config)

    def test_date_matches_daily_and_uses_daily_when_instrument_date_is_absent(self):
        config = self.config()
        del config["daily"]["instruments"]["000001.SZ"]["Date"]
        result = processing.make_profile(config)
        self.assertEqual(20260904, result["instruments"][0]["trading_date"])
        config["daily"]["instruments"]["000001.SZ"]["Date"] = 20260903
        with self.assertRaises(unified_config.ConfigError):
            processing.make_profile(config)

    def test_cli_writes_profile_without_requiring_model_files(self):
        config = self.config("SH")
        with tempfile.TemporaryDirectory() as directory:
            source = os.path.join(directory, "unified.json")
            output = os.path.join(directory, "processing.json")
            unified_config.write_json(source, config)
            for extra, mode in (([], "prediction"), (["--factors-only"], "factors-only")):
                processing.main(["--config", source, "--output", output] + extra)
                result = unified_config.load_json(output)
                self.assertEqual(mode, result["processing_mode"])
                self.assertEqual(config["prediction"], result["prediction"])
                self.assertEqual(config, unified_config.load_json(source))

    def test_cli_strategy_intents_are_explicit_and_invalid_combination_writes_nothing(self):
        config = self.config("SH")
        with tempfile.TemporaryDirectory() as directory:
            source = os.path.join(directory, "unified.json")
            output = os.path.join(directory, "processing.json")
            unified_config.write_json(source, config)
            with self.assertRaises(unified_config.ConfigError):
                processing.main(["--config", source, "--output", output,
                                 "--strategy-intents", "--factors-only"])
            self.assertFalse(os.path.exists(output))
            processing.main(["--config", source, "--output", output, "--strategy-intents"])
            profile = unified_config.load_json(output)
            self.assertEqual("paper-intents", profile["strategy_runtime"]["mode"])
            self.assertEqual(unified_config.export_legacy(config),
                             profile["strategy_runtime"]["legacy_config"])
            self.assertEqual(config, unified_config.load_json(source))


if __name__ == "__main__":
    unittest.main()
