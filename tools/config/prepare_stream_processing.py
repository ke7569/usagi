#!/usr/bin/env python3
"""Prepare a narrow, non-trading profile for native market stream tools.

The input must satisfy the existing unified schema, including its model-path
and complete static-field requirements. Factors-only mode skips model loading
in the consumer; it does not relax that schema. This tool never opens model
files, validates an artifact lock, loads a trading plugin, or connects to MD/TD.
Optional SH strategy intents carry the original ZStrategy configuration for
order-intent output only, without TD or simulated fills.
"""

import argparse
import copy
import os

from unified_config import ConfigError, export_legacy, load_json, processing_hash, validate, write_json


_BOOK_MODES = {
    "SZ": ("hp-realtime", "hp-shadow"),
    "SH": ("full-orderbook", "complete-orderbook-sh"),
}
_SNAPSHOT_PATHS = (
    "snapshot_baseline_model_path", "snapshot_baseline_scaler_path",
    "snapshot_auction59_model_path", "snapshot_auction59_scaler_path",
)
_PROCESSING_CONTRACTS = {"SZ": "sze-mix153060-v04", "SH": "sse-per-instrument-v2"}
_RECOVERY_FIELDS = {
    "enabled", "allow_invalid_replay_for_analysis", "trading_enabled",
    "trading_day", "source_id", "journal_directory", "journal_prefix",
    "journal_segment_mb", "journal_segment_bytes", "journal_max_payload_bytes",
    "shm_path", "expected_generation",
}
_UINT64_MAX = (1 << 64) - 1
_RECOVERY_HEADER_BYTES = 72
_RECOVERY_TRAILER_BYTES = 16
_RECOVERY_PAGE_BYTES = 4096
# Match the existing config_sse_hybrid_prediction_20260818.json contract.
_SSE_SAMPLING = {
    "mode": "hardware-gap-batch",
    "threshold_ns": 5000,
    "comparison": "greater-or-equal",
    "clock": "NIC_PHC",
    "candidate_event": "CompleteOrderBookSH Level2",
    "activity_scope": "per-subscription-sse-datagram-gap",
    "same_exchange_time_policy": "at-most-one-sample",
    "initial_window": "first-valid-book-at-or-after-open",
    "sequence_gap_policy": "fail-closed",
    "periodic_md": False,
    "shutdown_flush": False,
    "standard_gate": {
        "turnover_threshold_source": "daily-instrument-static-required",
        "exchange_time_trigger_us": 100000000,
        "mid_change_epsilon": 0.000001,
        "min_volume_change": 100,
    },
}
_SSE_ROUTING = {
    "clock": "exchange-time-of-day-micros",
    "snapshot_selected_window": "[09:30:00,09:35:00)",
    "tick_selected_window": "[09:35:00,24:00:00)",
    "tick_warm_before_switch": True,
    "silent_fallback": False,
}


def _require_contract_subset(declaration, expected, path):
    if not isinstance(declaration, dict):
        raise ConfigError(path + " must be an object")
    for key, value in declaration.items():
        name = path + "." + key
        if key not in expected:
            raise ConfigError("unsupported stream contract field: " + name)
        required = expected[key]
        if isinstance(required, dict):
            _require_contract_subset(value, required, name)
        elif type(value) is not type(required) or value != required:
            raise ConfigError("unsupported stream contract value: " + name)


def _recovery_projection(config, recovery_input):
    if recovery_input is None:
        return None, None
    if config["market"] != "SZ":
        raise ConfigError("recovery_input is only supported for SZ")
    if recovery_input not in ("journal", "handoff"):
        raise ConfigError("recovery_input must be journal or handoff")
    recovery = config.get("market_data", {}).get("recovery")
    if not isinstance(recovery, dict):
        raise ConfigError("market_data.recovery is required for recovery_input")
    unknown = set(recovery) - _RECOVERY_FIELDS
    if unknown:
        raise ConfigError("unknown recovery option: " + ", ".join(sorted(unknown)))
    if recovery.get("enabled") is not True:
        raise ConfigError("recovery input requires market_data.recovery.enabled=true")
    if recovery.get("trading_enabled", False) is not False:
        raise ConfigError("recovery input requires trading_enabled=false")
    if type(recovery.get("trading_day")) is not int or \
            recovery["trading_day"] != config["daily"]["trading_day"]:
        raise ConfigError("recovery trading_day must match daily.trading_day")
    if type(recovery.get("source_id")) is not int or not 0 < recovery["source_id"] <= 65535:
        raise ConfigError("recovery source_id must be a uint16")
    for field in ("journal_directory", "journal_prefix"):
        if not isinstance(recovery.get(field), str) or not recovery[field]:
            raise ConfigError("recovery." + field + " is required")
    if type(recovery.get("journal_max_payload_bytes")) is not int or not \
            72 <= recovery["journal_max_payload_bytes"] <= 65535:
        raise ConfigError("recovery.journal_max_payload_bytes must be in [72,65535]")
    segment_mb = recovery.get("journal_segment_mb")
    segment_bytes = recovery.get("journal_segment_bytes")
    if segment_mb is not None and segment_bytes is not None:
        raise ConfigError("recovery journal_segment_mb and journal_segment_bytes are mutually exclusive")
    if segment_mb is None and segment_bytes is None:
        raise ConfigError("recovery requires journal_segment_mb or journal_segment_bytes")
    max_payload = recovery["journal_max_payload_bytes"]
    minimum_segment = _RECOVERY_PAGE_BYTES + _RECOVERY_HEADER_BYTES + \
        max_payload + _RECOVERY_TRAILER_BYTES
    if segment_mb is not None:
        if type(segment_mb) is not int or segment_mb <= 0 or \
                segment_mb * 1024 * 1024 > _UINT64_MAX or \
                segment_mb * 1024 * 1024 < minimum_segment:
            raise ConfigError("recovery.journal_segment_mb is outside the valid range")
    if segment_bytes is not None:
        if type(segment_bytes) is not int or segment_bytes < minimum_segment or \
                segment_bytes > _UINT64_MAX:
            raise ConfigError("recovery.journal_segment_bytes is outside the valid range")
    if "expected_generation" in recovery and (
            type(recovery["expected_generation"]) is not int or
            not 0 < recovery["expected_generation"] <= _UINT64_MAX):
        raise ConfigError("recovery.expected_generation must be a positive uint64")
    if "allow_invalid_replay_for_analysis" in recovery:
        if type(recovery["allow_invalid_replay_for_analysis"]) is not bool:
            raise ConfigError("recovery.allow_invalid_replay_for_analysis must be boolean")
        if recovery["allow_invalid_replay_for_analysis"]:
            raise ConfigError("recovery does not allow invalid replay bypass")
    environment = config["environment"]
    if recovery_input == "journal":
        if environment.get("mode") != "replay" or environment.get("record_format") != "canonical-events" or \
                environment.get("time_basis") != "exchange":
            raise ConfigError("journal recovery requires replay canonical-events with exchange time")
        if os.path.abspath(os.path.normpath(environment["recording"])) != \
                os.path.abspath(os.path.normpath(recovery["journal_directory"])):
            raise ConfigError("journal replay recording must match recovery.journal_directory")
    else:
        if environment.get("mode") != "live":
            raise ConfigError("handoff recovery requires live environment")
        if not isinstance(recovery.get("shm_path"), str) or not recovery["shm_path"]:
            raise ConfigError("handoff recovery requires recovery.shm_path")
        if type(recovery.get("expected_generation")) is not int or recovery["expected_generation"] <= 0:
            raise ConfigError("handoff recovery requires positive recovery.expected_generation")
    projected = copy.deepcopy(recovery)
    projected.setdefault("trading_enabled", False)
    return projected, "sze-" + recovery_input


def make_profile(config, factors_only=False, strategy_intents=False,
                 recovery_input=None):
    """Validate a unified config and project only stream-processing inputs."""
    if type(factors_only) is not bool:
        raise ConfigError("factors_only must be boolean")
    if type(strategy_intents) is not bool:
        raise ConfigError("strategy_intents must be boolean")
    validate(config)
    market = config["market"]
    if strategy_intents and factors_only:
        raise ConfigError("strategy intents require prediction mode")
    environment = config["environment"]
    if environment["mode"] not in ("live", "replay"):
        raise ConfigError("stream processing requires a bound live or replay environment")
    if environment["execution"] != "disabled":
        raise ConfigError("stream processing requires environment.execution=disabled")
    if strategy_intents and "oms" not in config["trading"]:
        raise ConfigError("strategy intents require trading.oms")
    if environment["mode"] == "replay" and (
            environment["record_format"] != "t0md-v1" or
            environment["time_basis"] != "recorded-receive") and not (
                recovery_input == "journal" and
                environment["record_format"] == "canonical-events" and
                environment["time_basis"] == "exchange"):
        raise ConfigError("stream replay requires t0md-v1 with recorded-receive time")

    prediction = config["prediction"]
    if "book_mode" in prediction and prediction["book_mode"] not in _BOOK_MODES[market]:
        raise ConfigError("unsupported {} prediction.book_mode".format(market))
    if market == "SH" and "model_type" in prediction and prediction["model_type"] != "sse_hybrid_native":
        raise ConfigError("SH stream processing requires model_type=sse_hybrid_native")
    if market == "SH":
        for field, expected in (("sampling", _SSE_SAMPLING), ("routing", _SSE_ROUTING)):
            if field in prediction:
                _require_contract_subset(prediction[field], expected, "prediction." + field)
    required_paths = ("model_path",) + (_SNAPSHOT_PATHS if market == "SH" else ())
    for name in required_paths:
        if not isinstance(prediction.get(name), str) or not prediction[name].strip():
            raise ConfigError("stream processing requires prediction." + name)

    trading_day = config["daily"]["trading_day"]
    instruments = []
    for symbol, values in sorted(config["daily"]["instruments"].items()):
        threshold = values["HistoryAmount"] / 8000.0
        if threshold <= 0.0:
            raise ConfigError(symbol + " has a non-positive derived turnover_threshold")
        instruments.append({
            "instrument": symbol[:-3],
            "trading_date": values.get("Date", trading_day),
            "average_amount": values["HistoryAmount"],
            "turnover_threshold": threshold,
            "free_share": values["FreeShare"] if "FreeShare" in values else values["free_share"],
            "pre_close": values["Close"],
            "upper_limit": values["HpUpperPrice"],
            "lower_limit": values["HpLowerPrice"],
            "history_volatility_20d": values["HistoryVolatility20d"],
        })
        if market == "SH":
            for field in ("listing_date", "is_ipo_first_day"):
                if field in values:
                    instruments[-1][field] = values[field]
    profile = {
        "schema_version": 1,
        "market": market,
        "execution": "disabled",
        "processing_mode": "factors-only" if factors_only else "prediction",
        "processing_contract": _PROCESSING_CONTRACTS[market],
        "trading_day": trading_day,
        "processing_sha256": processing_hash(config),
        "environment": copy.deepcopy(environment),
        "prediction": copy.deepcopy(prediction),
        "instruments": instruments,
    }
    recovery, input_driver = _recovery_projection(config, recovery_input)
    if recovery is not None:
        profile["recovery"] = recovery
        profile["input_driver"] = input_driver
    if strategy_intents:
        profile["strategy_runtime"] = {
            "mode": "paper-intents",
            "account_reference": config["account"]["reference"],
            "oms": copy.deepcopy(config["trading"]["oms"]),
            "legacy_config": export_legacy(config),
        }
    return profile


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--factors-only", action="store_true",
                        help="skip model loading in the consumer, retaining schema requirements")
    parser.add_argument("--strategy-intents", action="store_true",
                        help="enable paper ZStrategy order-intent output only, without TD or simulated fills")
    parser.add_argument("--recovery-input", choices=("journal", "handoff"))
    args = parser.parse_args(argv)
    profile = make_profile(load_json(args.config), factors_only=args.factors_only,
                           strategy_intents=args.strategy_intents,
                           recovery_input=args.recovery_input)
    write_json(args.output, profile)


if __name__ == "__main__":
    try:
        main()
    except (ConfigError, ValueError, TypeError, OSError) as error:
        raise SystemExit("prepare_stream_processing_error: " + str(error))
