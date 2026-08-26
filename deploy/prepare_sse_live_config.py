#!/usr/bin/env python3
"""Build a Shanghai live strategy config from one daily static JSON.

The generated strategy config contains no TD credentials.  Credentials belong
to the root-only Deepwin TD account config loaded by the launcher.
"""

from __future__ import print_function

import argparse
import copy
import json
import os
import re
import tempfile


SYMBOL_RE = re.compile(r"^[0-9]{6}\.SH$")
REQUIRED = ("Close", "HistoryAmount", "FreeShare", "HpUpperPrice",
            "HpLowerPrice", "HistoryVolatility20d", "static_position")


class ConfigError(Exception):
    pass


def load_json(path):
    with open(path, "r") as stream:
        return json.load(stream)


def write_json(path, value):
    parent = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(parent):
        os.makedirs(parent)
    fd, temporary = tempfile.mkstemp(prefix=".sse-config-", dir=parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.rename(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def validate_daily(daily, expected_day):
    if not isinstance(daily, dict) or not isinstance(daily.get("ins_params"), dict):
        raise ConfigError("daily JSON must contain a non-empty ins_params object")
    if not daily["ins_params"]:
        raise ConfigError("daily ins_params is empty")
    day = int(daily.get("trading_day", expected_day))
    if expected_day is not None and day != expected_day:
        raise ConfigError("trading_day {} does not match {}".format(day, expected_day))
    for symbol, item in daily["ins_params"].items():
        if not SYMBOL_RE.match(symbol):
            raise ConfigError("non-SSE stock in daily config: {}".format(symbol))
        if not isinstance(item, dict):
            raise ConfigError("ins_params.{} is not an object".format(symbol))
        for key in REQUIRED:
            if key not in item:
                raise ConfigError("ins_params.{} missing {}".format(symbol, key))
        if int(item.get("Date", day)) != day:
            raise ConfigError("ins_params.{}.Date does not match trading_day".format(symbol))
        if float(item["HistoryAmount"]) <= 0.0 or float(item["FreeShare"]) <= 0.0:
            raise ConfigError("ins_params.{} requires positive HistoryAmount/FreeShare".format(symbol))
    return day


def build(template, daily, model_root, routing_path, day):
    config = copy.deepcopy(template) if template else {}
    params = copy.deepcopy(daily["ins_params"])
    for item in params.values():
        item["Date"] = day
        item.setdefault("last_position", 0)
        item.pop("cpu", None)
    config.update({
        "strategy_name": "sse_t0_live_{}".format(day),
        "name": "HStrategy",
        "market": "SH",
        "trading_day": day,
        "runtime_mode": "live",
        "prediction_only": False,
        "trading_enabled": True,
        # Explicit operator action is still required before any order.
        "production_approval": bool(config.get("production_approval", False)),
        "instrument_universe_mode": "all-decoded-sse-equities",
        "instrument_id": sorted(params),
        "ins_params": params,
        "td_source_index": [190],
        "model_type": "sse_hybrid_native",
        "model_path": os.path.join(model_root, "tick", "ssemodl1.bin"),
        "snapshot_baseline_model_path": os.path.join(model_root, "snapshot", "baseline.ssegru"),
        "snapshot_baseline_scaler_path": os.path.join(model_root, "snapshot", "baseline.json"),
        "snapshot_auction59_model_path": os.path.join(model_root, "snapshot", "auction59.ssegru"),
        "snapshot_auction59_scaler_path": os.path.join(model_root, "snapshot", "auction59.json"),
        "sse_hybrid_routing_path": routing_path or os.path.join(
            model_root, "sse_hybrid_routing.json"),
        "sse_factor_contract": "v0.4-sse-cob-batch-end-100us",
        "sse_inference_backend": "native-cpp",
        "sse_order_routing": {
            "enabled": True,
            "mode": "live",
            "td_source": 190,
            "position_query_retry_ms": 5000,
            "position_query_cutoff_hhmmss": 93100,
        },
        "sse_test_order": {
            "enabled": False,
            "instrument": "",
            "side": "buy",
            "price": 0.0,
            "volume": 100,
            "trigger_after_signals": 1,
            "cancel_delay_ms": 1000,
        },
        "model_routing": {
            "clock": "exchange-time-of-day-micros",
            "snapshot_selected_window": "[09:30:00,09:35:00)",
            "tick_selected_window": "[09:35:00,24:00:00)",
            "tick_warm_before_switch": True,
            "silent_fallback": False,
        },
        "sse_live_sampling": {
            "mode": "trailing-edge-one-shot",
            "threshold_ns": 100000,
            "comparison": "strict-greater-than",
            "clock": "CLOCK_MONOTONIC",
            "candidate_event": "CompleteOrderBookSH Level2",
            "activity_scope": "normalized-sse-book-update",
            "sequence_gap_policy": "fail-closed",
            "periodic_md": False,
            "shutdown_flush": False,
        },
        "feed_health": {
            "require_sequence_healthy": True,
            "sample_on_gap": False,
            "resume_after_explicit_resync": True,
        },
    })
    return config


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--daily", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--template")
    parser.add_argument("--model-root", default="/home/zane/sse/artifacts")
    parser.add_argument("--routing-path")
    parser.add_argument("--trading-day", type=int)
    args = parser.parse_args()
    daily = load_json(args.daily)
    day = validate_daily(daily, args.trading_day)
    template = load_json(args.template) if args.template else {}
    config = build(template, daily, args.model_root, args.routing_path, day)
    write_json(args.output, config)
    print("generated {} instruments={} production_approval={}".format(
        args.output, len(config["ins_params"]), config["production_approval"]))


if __name__ == "__main__":
    try:
        main()
    except (ConfigError, IOError, ValueError) as error:
        raise SystemExit("sse_config_error: {}".format(error))
