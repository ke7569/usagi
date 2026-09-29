#!/usr/bin/env python3
"""Offline, lossless migration of resolved SZE/SSE strategy configuration.

This tool never loads a trading plugin or opens a market-data connection.
The environment contract is consumed by future common runtime adapters.
"""

import argparse
import copy
import datetime
import hashlib
import importlib.util
import inspect
import json
import math
import os
import re
import tempfile


class ConfigError(ValueError):
    pass


STATIC_FIELDS = {
    "Date", "Close", "Amount", "Range", "HistoryAmount", "FreeShare",
    "free_share", "HpUpperPrice", "HpLowerPrice", "HpFeeShare",
    "HistoryVolatility20d", "listing_date", "is_ipo_first_day",
}
POSITION_FIELDS = {"static_position", "last_position", "external_delta"}
INSTRUMENT_FIELDS = {"vol_unit", "min_order_size", "max_order_size"}
SECRET_FIELDS = {
    "password", "trade_password", "agw_password", "login_password",
    "secret", "private_key", "api_key", "access_token",
}
SECTIONS = {
    "schema_version", "market", "daily", "strategy", "prediction",
    "market_data", "trading", "account", "deployment", "legacy",
    "environment", "migration",
}

# Alias presence is retained in migration.top_fields, including absent fields.
FIELDS = {
    "trading_day": ("daily", "trading_day"),
    "static_data_source_date": ("daily", "source_date"),
    "static_data_hash": ("migration", "source_static_hash"),
    "strategy_name": ("strategy", "name"),
    "global_params": ("strategy", "parameters"),
    "sze_startup_warmup_signals": ("strategy", "warmup_signals"),
    "model_path": ("prediction", "model_path"),
    "model_type": ("prediction", "model_type"),
    "mix153060_model_sha256": ("prediction", "model_sha256"),
    "mode": ("prediction", "book_mode"),
    "sz_orderbook_mode": ("prediction", "book_mode"),
    "sh_orderbook_mode": ("prediction", "book_mode"),
    "orderbook_mode": ("prediction", "book_mode"),
    "snapshot_legacy15": ("prediction", "snapshot_legacy15"),
    "model_routing": ("prediction", "routing"),
    "sse_hybrid_routing_path": ("prediction", "routing_path"),
    "sse_factor_contract": ("prediction", "factor_contract"),
    "sse_inference_backend": ("prediction", "inference_backend"),
    "sse_live_sampling": ("prediction", "sampling"),
    "sze_prediction_capture": ("prediction", "capture"),
    "mix153060_capture": ("prediction", "capture"),
    "prediction_capture": ("prediction", "capture"),
    "md_source_index": ("market_data", "source_ids"),
    "snapshot_source_id": ("market_data", "snapshot_source_id"),
    "sze_recovery_consumer": ("market_data", "recovery"),
    "feed_health": ("market_data", "health"),
    "td_source_index": ("trading", "source_ids"),
    "runtime_mode": ("trading", "runtime_mode"),
    "prediction_only": ("trading", "prediction_only"),
    "trading_enabled": ("trading", "enabled"),
    "production_approval": ("trading", "production_approval"),
    "oms": ("trading", "oms"),
    "name": ("deployment", "entry_class"),
    "vtd": ("deployment", "vtd"),
    "instrument_universe_mode": ("deployment", "universe_mode"),
    "sse_prediction_log_path": ("deployment", "prediction_log_path"),
}
for _name in ("snapshot_baseline_model_path", "snapshot_baseline_scaler_path",
              "snapshot_auction59_model_path", "snapshot_auction59_scaler_path",
              "snapshot_auction59_factors_path", "hp_fee_share", "hp_downsample",
              "hp_upper_price", "hp_lower_price", "hp_capture_failure_digest"):
    FIELDS[_name] = ("prediction", _name)
for _name in ("instrument_id", "his_amt", "static_position", "last_position",
              "deployment_required"):
    FIELDS[_name] = ("legacy", _name)


def canonical_hash(value):
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True, allow_nan=False).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def file_hash(path):
    if not os.path.isfile(path):
        raise ConfigError("expected a regular artifact file: " + str(path))
    result = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def load_json(path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ConfigError("duplicate JSON key: " + key)
            result[key] = value
        return result

    def constant(value):
        raise ConfigError("non-finite JSON number: " + value)

    with open(path, encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=pairs, parse_constant=constant)


def write_json(path, value):
    parent = os.path.dirname(os.path.abspath(path))
    os.makedirs(parent, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".unified-config-", dir=parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, sort_keys=True, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def require_object(value, path):
    if not isinstance(value, dict):
        raise ConfigError(path + " must be an object")


def number(value, path, minimum=None, integer=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigError(path + " must be numeric")
    if not math.isfinite(value) or (integer and int(value) != value):
        raise ConfigError(path + " must be a finite" + (" integer" if integer else " number"))
    if minimum is not None and value < minimum:
        raise ConfigError(path + " is below its minimum")


def oms_money(value, path):
    number(value, path, minimum=0)
    scaled = value * 10000.0
    if abs(scaled - round(scaled)) > 1e-7:
        raise ConfigError(path + " must use at most 1/10000 RMB precision")
    if value > 1000000000000.0:
        raise ConfigError(path + " exceeds the 1e12 RMB limit")


def validate_oms(value, path="trading.oms"):
    require_object(value, path)
    required = {"simulation_cash", "fee_reserve_per_order"}
    unknown = set(value) - required
    if unknown:
        raise ConfigError("unknown OMS fields: " + ", ".join(sorted(unknown)))
    missing = required - set(value)
    if missing:
        raise ConfigError("missing OMS fields: " + ", ".join(sorted(missing)))
    oms_money(value["simulation_cash"], path + ".simulation_cash")
    oms_money(value["fee_reserve_per_order"], path + ".fee_reserve_per_order")


def date_value(value, path):
    if type(value) is not int:
        raise ConfigError(path + " must be an integer YYYYMMDD")
    try:
        parsed = datetime.datetime.strptime(str(value), "%Y%m%d")
    except ValueError:
        raise ConfigError(path + " must be a valid YYYYMMDD")
    if len(str(value)) != 8 or parsed.year < 2000:
        raise ConfigError(path + " must be a valid YYYYMMDD")


def check_json(value, path="config"):
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise ConfigError(path + " keys must be strings")
            if key.lower() in SECRET_FIELDS:
                raise ConfigError(path + "." + key + ": use an external credential reference")
            check_json(item, path + "." + key)
    elif isinstance(value, list):
        for item in value:
            check_json(item, path + "[]")
    elif isinstance(value, float) and not math.isfinite(value):
        raise ConfigError(path + " contains a non-finite number")
    elif value is not None and not isinstance(value, (str, int, float, bool)):
        raise ConfigError(path + " is not JSON data")


def validate_options(value, path):
    """Type-check known legacy controls while retaining adapter-owned fields."""
    booleans = {"enabled", "trading_enabled", "production_approval", "prediction_only",
                "capture_only", "allow_invalid_replay_for_analysis", "shutdown_flush",
                "periodic_md", "sample_on_gap", "resume_after_explicit_resync",
                "require_sequence_healthy", "catchup_gate_enabled", "silent_fallback",
                "tick_warm_before_switch"}
    integers = {"threshold_ns", "catchup_gate_threshold_ms", "position_query_retry_ms",
                "cancel_delay_ms", "trigger_after_signals", "source_id", "td_source",
                "max_position", "max_order_volume", "state_cpu", "strategy_cpu",
                "journal_segment_mb", "shm_capacity", "handoff_timeout_ms", "volume"}
    for key, item in value.items():
        name = path + "." + key
        if key in booleans and type(item) is not bool:
            raise ConfigError(name + " must be boolean")
        if key in integers:
            number(item, name, minimum=0, integer=True)
        if key == "price":
            number(item, name, minimum=0)
        if isinstance(item, dict):
            validate_options(item, name)


def fields_for(market):
    result = dict(FIELDS)
    prefix = "sze" if market == "SZ" else "sse"
    result[prefix + "_order_routing"] = ("trading", "routing")
    result[prefix + "_test_order"] = ("trading", "test_order")
    return result


def symbol_key(symbol, market):
    if not isinstance(symbol, str):
        raise ConfigError("instrument ID must be a string")
    code = symbol[:-3] if symbol.endswith("." + market) else symbol
    prefixes = ("00", "30") if market == "SZ" else ("60", "68")
    if not re.fullmatch(r"[0-9]{6}", code) or not code.startswith(prefixes):
        raise ConfigError("invalid {} instrument: {}".format(market, symbol))
    return code + "." + market


def migrate_runtime(runtime, account_ref, day=None):
    require_object(runtime, "runtime")
    check_json(runtime)
    if runtime.get("market") not in ("SZ", "SH"):
        raise ConfigError("runtime.market must be SZ or SH")
    market = runtime["market"]
    fields = fields_for(market)
    unknown = set(runtime) - set(fields) - {"market", "ins_params"}
    if unknown:
        raise ConfigError("unmapped runtime fields: " + ", ".join(sorted(unknown)))
    params = runtime.get("ins_params")
    require_object(params, "runtime.ins_params")
    if not params:
        raise ConfigError("runtime.ins_params must not be empty")
    dates = set()
    for item in params.values():
        if isinstance(item, dict) and "Date" in item:
            date_value(item["Date"], "ins_params.Date")
            dates.add(item["Date"])
    selected_day = runtime.get("trading_day", day)
    if selected_day is None and len(dates) == 1:
        selected_day = next(iter(dates))
    date_value(selected_day, "trading_day (supply --day if absent)")
    if day is not None and selected_day != day:
        raise ConfigError("trading_day disagrees with --day")
    config = {name: {} for name in SECTIONS}
    config.update({"schema_version": 1, "market": market})
    config["daily"] = {"trading_day": selected_day, "instruments": {}}
    config["strategy"] = {"instrument_parameters": {}}
    config["account"] = {"reference": account_ref, "positions": {}}
    config["deployment"] = {"instrument_cpu": {}}
    config["environment"] = {"mode": "unbound", "execution": "disabled"}
    config["migration"] = {
        "top_fields": sorted(runtime), "instrument_keys": {},
        "source_runtime_sha256": canonical_hash(runtime),
    }
    for key, value in runtime.items():
        if key not in fields:
            continue
        section, field = fields[key]
        if field in config[section] and config[section][field] != value:
            raise ConfigError("conflicting aliases for {}.{}".format(section, field))
        config[section][field] = copy.deepcopy(value)
    for raw_symbol, values in params.items():
        require_object(values, "ins_params." + raw_symbol)
        symbol = symbol_key(raw_symbol, market)
        if symbol in config["daily"]["instruments"]:
            raise ConfigError("duplicate normalized instrument: " + symbol)
        unknown = set(values) - STATIC_FIELDS - POSITION_FIELDS - INSTRUMENT_FIELDS - {"cpu"}
        if unknown:
            raise ConfigError("unmapped instrument fields: " + ", ".join(sorted(unknown)))
        config["migration"]["instrument_keys"][symbol] = raw_symbol
        config["daily"]["instruments"][symbol] = {
            key: copy.deepcopy(value) for key, value in values.items() if key in STATIC_FIELDS}
        config["account"]["positions"][symbol] = {
            key: value for key, value in values.items() if key in POSITION_FIELDS}
        config["strategy"]["instrument_parameters"][symbol] = {
            key: value for key, value in values.items() if key in INSTRUMENT_FIELDS}
        if "cpu" in values:
            config["deployment"]["instrument_cpu"][symbol] = values["cpu"]
    validate(config)
    return config


def _export(config):
    fields = fields_for(config["market"])
    output = {}
    for key in config["migration"]["top_fields"]:
        if key == "market":
            output[key] = config["market"]
        elif key == "ins_params":
            output[key] = {}
        else:
            section, field = fields[key]
            output[key] = copy.deepcopy(config[section][field])
    for symbol, values in config["daily"]["instruments"].items():
        item = copy.deepcopy(values)
        item.update(config["account"]["positions"][symbol])
        item.update(config["strategy"]["instrument_parameters"][symbol])
        if symbol in config["deployment"]["instrument_cpu"]:
            item["cpu"] = config["deployment"]["instrument_cpu"][symbol]
        output["ins_params"][config["migration"]["instrument_keys"][symbol]] = item
    return output


def validate(config):
    require_object(config, "config")
    check_json(config)
    if set(config) != SECTIONS or type(config["schema_version"]) is not int or config["schema_version"] != 1:
        raise ConfigError("expected unified schema_version=1 with the documented sections")
    if config["market"] not in ("SZ", "SH"):
        raise ConfigError("market must be SZ or SH")
    for section in SECTIONS - {"schema_version", "market"}:
        require_object(config[section], section)
    reference = config["account"].get("reference")
    if not isinstance(reference, str) or not reference.strip():
        raise ConfigError("account.reference must be an explicit non-empty reference")
    fields = fields_for(config["market"])
    top_fields = config["migration"].get("top_fields")
    if not isinstance(top_fields, list) or any(not isinstance(key, str) for key in top_fields):
        raise ConfigError("migration.top_fields must be a string array")
    if len(top_fields) != len(set(top_fields)) or not {"market", "ins_params"}.issubset(top_fields):
        raise ConfigError("migration.top_fields is incomplete or contains duplicates")
    if set(top_fields) - set(fields) - {"market", "ins_params"}:
        raise ConfigError("migration contains unmapped legacy fields")
    allowed = {section: set() for section in SECTIONS}
    for key in top_fields:
        if key in fields:
            section, field = fields[key]
            allowed[section].add(field)
    allowed["daily"].update(("instruments", "trading_day"))
    allowed["strategy"].add("instrument_parameters")
    allowed["account"].update(("reference", "positions"))
    allowed["deployment"].add("instrument_cpu")
    allowed["migration"].update(("top_fields", "instrument_keys", "source_runtime_sha256", "composition"))
    for section in allowed:
        if section in ("schema_version", "market", "environment"):
            continue
        if set(config[section]) - allowed[section]:
            raise ConfigError("unmapped fields in " + section)
    for key in top_fields:
        if key in fields:
            section, field = fields[key]
            if field not in config[section]:
                raise ConfigError("missing {}.{}".format(section, field))
    digest = config["migration"].get("source_runtime_sha256")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ConfigError("migration.source_runtime_sha256 must be a SHA-256")
    daily = config["daily"]
    date_value(daily.get("trading_day"), "daily.trading_day")
    if "source_date" in daily:
        date_value(daily["source_date"], "daily.source_date")
        if daily["source_date"] > daily["trading_day"]:
            raise ConfigError("source_date is after trading_day")
    instruments = daily.get("instruments")
    require_object(instruments, "daily.instruments")
    if not instruments:
        raise ConfigError("daily.instruments must not be empty")
    for section, key in (("account", "positions"), ("strategy", "instrument_parameters"),
                         ("migration", "instrument_keys")):
        require_object(config[section].get(key), section + "." + key)
        if set(config[section][key]) != set(instruments):
            raise ConfigError(section + "." + key + " instrument set mismatch")
    require_object(config["deployment"].get("instrument_cpu"), "deployment.instrument_cpu")
    if set(config["deployment"]["instrument_cpu"]) - set(instruments):
        raise ConfigError("CPU assignment for unknown instrument")
    for symbol, values in instruments.items():
        if symbol_key(symbol, config["market"]) != symbol:
            raise ConfigError("unified instrument IDs must include the market suffix")
        if symbol_key(config["migration"]["instrument_keys"][symbol], config["market"]) != symbol:
            raise ConfigError("migration instrument key mismatch")
        require_object(values, "daily.instruments." + symbol)
        if set(values) - STATIC_FIELDS:
            raise ConfigError("non-static field in daily.instruments." + symbol)
        for key in ("Close", "HistoryAmount", "HpUpperPrice", "HpLowerPrice", "HistoryVolatility20d"):
            if key not in values:
                raise ConfigError(symbol + " missing " + key)
        if "FreeShare" not in values and "free_share" not in values:
            raise ConfigError(symbol + " missing FreeShare")
        if "FreeShare" in values and "free_share" in values and values["FreeShare"] != values["free_share"]:
            raise ConfigError(symbol + " has conflicting FreeShare aliases")
        for key, value in values.items():
            if key == "Date":
                date_value(value, symbol + ".Date")
                if value != daily["trading_day"]:
                    raise ConfigError(symbol + ".Date differs from trading_day")
            elif key == "listing_date":
                date_value(value, symbol + ".listing_date")
                if value > daily["trading_day"]:
                    raise ConfigError(symbol + ".listing_date is after trading_day")
            elif key == "is_ipo_first_day":
                if type(value) is not bool:
                    raise ConfigError(symbol + ".is_ipo_first_day must be boolean")
            else:
                number(value, symbol + "." + key, minimum=0)
        if "listing_date" in values and "is_ipo_first_day" in values and \
                values["is_ipo_first_day"] != (values["listing_date"] == daily["trading_day"]):
            raise ConfigError(symbol + " has inconsistent listing date and first-day flag")
        for key in ("Close", "HistoryAmount", "FreeShare", "free_share", "HpUpperPrice", "HpLowerPrice"):
            if key in values and values[key] <= 0:
                raise ConfigError(symbol + "." + key + " must be positive")
        if values["HpLowerPrice"] > values["HpUpperPrice"]:
            raise ConfigError(symbol + " has inverted price limits")
        positions = config["account"]["positions"][symbol]
        rules = config["strategy"]["instrument_parameters"][symbol]
        for obj, allowed_keys in ((positions, POSITION_FIELDS), (rules, INSTRUMENT_FIELDS)):
            require_object(obj, symbol)
            if set(obj) - allowed_keys:
                raise ConfigError(symbol + " has misplaced instrument fields")
            for key, value in obj.items():
                number(value, symbol + "." + key,
                       minimum=None if key in ("last_position", "external_delta") else 0,
                       integer=key in POSITION_FIELDS or key == "vol_unit")
                if key == "vol_unit" and value <= 0:
                    raise ConfigError("vol_unit must be positive")
        if "external_delta" in positions:
            delta = positions["external_delta"]
            bottom = positions.get("static_position", 0)
            previous = bottom - delta
            actual = previous + positions.get("last_position", 0)
            if abs(delta) > 2147483647 or previous < 0 or not 0 <= actual <= 2147483647:
                raise ConfigError(symbol + " has invalid pre-execution holdings")
        if symbol in config["deployment"]["instrument_cpu"]:
            number(config["deployment"]["instrument_cpu"][symbol], "cpu", minimum=0, integer=True)
    parameters = config["strategy"].get("parameters", {})
    require_object(parameters, "strategy.parameters")
    for key, value in parameters.items():
        number(value, "strategy.parameters." + key)
    for section, keys in (("strategy", ("name",)),
                          ("deployment", ("entry_class", "universe_mode", "prediction_log_path")),
                          ("prediction", ("book_mode", "model_type", "factor_contract", "inference_backend"))):
        for key in keys:
            if key in config[section] and (not isinstance(config[section][key], str) or not config[section][key]):
                raise ConfigError(section + "." + key + " must be a non-empty string")
    if "warmup_signals" in config["strategy"]:
        number(config["strategy"]["warmup_signals"], "warmup_signals", 0, True)
    for section in ("market_data", "trading"):
        sources = config[section].get("source_ids", [])
        if not isinstance(sources, list) or any(type(value) is not int or not 0 < value <= 32767 for value in sources):
            raise ConfigError(section + ".source_ids must contain positive integers")
        if len(sources) != len(set(sources)):
            raise ConfigError(section + ".source_ids contains duplicates")
    trading = config["trading"]
    if "oms" in trading:
        validate_oms(trading["oms"])
    for key in ("enabled", "prediction_only", "production_approval"):
        if key in trading and type(trading[key]) is not bool:
            raise ConfigError("trading." + key + " must be boolean")
    for key in ("routing", "test_order"):
        if key in trading:
            require_object(trading[key], "trading." + key)
            if "enabled" in trading[key] and type(trading[key]["enabled"]) is not bool:
                raise ConfigError("trading." + key + ".enabled must be boolean")
    if trading.get("prediction_only") and (trading.get("enabled") or trading.get("source_ids")):
        raise ConfigError("prediction_only conflicts with trading/TD sources")
    if trading.get("enabled") and not trading.get("source_ids"):
        raise ConfigError("trading.enabled requires an explicit TD source")
    if "runtime_mode" in trading and trading["runtime_mode"] not in ("live", "prediction-only", "staged"):
        raise ConfigError("unsupported trading.runtime_mode")
    routing = trading.get("routing", {})
    if "mode" in routing and routing["mode"] not in ("live", "virtual"):
        raise ConfigError("routing.mode must be live or virtual")
    if routing.get("enabled") and routing.get("mode") == "live" and not trading.get("source_ids"):
        raise ConfigError("live routing requires an explicit TD source")
    if "td_source" in routing and trading.get("source_ids") != [routing["td_source"]]:
        raise ConfigError("routing.td_source disagrees with TD source_ids")
    prediction = config["prediction"]
    for section in ("prediction", "market_data", "trading"):
        validate_options(config[section], section)
    for key in ("routing", "sampling", "capture", "snapshot_legacy15"):
        if key in prediction:
            require_object(prediction[key], "prediction." + key)
    for key in ("recovery", "health"):
        if key in config["market_data"]:
            require_object(config["market_data"][key], "market_data." + key)
    for key, value in prediction.items():
        if key.endswith("_path") and (not isinstance(value, str) or not value):
            raise ConfigError("prediction." + key + " must be a non-empty path string")
    if not isinstance(prediction.get("model_path"), str) or not prediction["model_path"]:
        raise ConfigError("prediction.model_path must be explicit")
    if config["market"] == "SH" and prediction.get("model_type") == "sse_hybrid_native":
        for field in ("snapshot_baseline_model_path", "snapshot_baseline_scaler_path",
                      "snapshot_auction59_model_path", "snapshot_auction59_scaler_path"):
            if not isinstance(prediction.get(field), str) or not prediction[field]:
                raise ConfigError("hybrid model requires " + field)
    if "model_sha256" in prediction and (not isinstance(prediction["model_sha256"], str) or
            not re.fullmatch(r"[0-9a-f]{64}", prediction["model_sha256"])):
        raise ConfigError("prediction.model_sha256 must be a lowercase SHA-256")
    _validate_legacy_arrays(config)
    source_hash = config["migration"].get("source_static_hash")
    if source_hash is not None and source_hash != canonical_hash(_export(config)["ins_params"]):
        raise ConfigError("source static_data_hash mismatch")
    _validate_environment(config["environment"])
    return config


def _validate_legacy_arrays(config):
    legacy = config["legacy"]
    ids = legacy.get("instrument_id", [])
    if not isinstance(ids, list):
        raise ConfigError("legacy.instrument_id must be an array")
    symbols = [symbol_key(value, config["market"]) for value in ids]
    if len(symbols) != len(set(symbols)):
        raise ConfigError("legacy.instrument_id contains duplicates")
    if symbols and set(symbols) != set(config["daily"]["instruments"]):
        raise ConfigError("legacy.instrument_id disagrees with ins_params universe")
    for field in ("his_amt", "static_position", "last_position"):
        values = legacy.get(field, [])
        if not isinstance(values, list) or (values and len(values) != len(symbols)):
            raise ConfigError("legacy." + field + " length disagrees with instrument_id")
        for symbol, value in zip(symbols, values):
            number(value, "legacy." + field, minimum=None if field == "last_position" else 0,
                   integer=field in POSITION_FIELDS)
            original = (config["daily"]["instruments"][symbol].get("HistoryAmount")
                        if field == "his_amt" else config["account"]["positions"][symbol].get(field))
            if original is None or original != value:
                raise ConfigError("legacy." + field + " disagrees with ins_params for " + symbol)


def _validate_environment(environment):
    mode = environment.get("mode")
    if mode not in ("unbound", "live", "replay"):
        raise ConfigError("environment.mode must be unbound, live or replay")
    allowed = {"mode", "execution", "recording", "record_format", "time_basis", "clock"}
    if set(environment) - allowed:
        raise ConfigError("unknown environment fields")
    if environment.get("execution") not in ("disabled", "paper", "live"):
        raise ConfigError("invalid execution adapter")
    if mode != "live" and environment["execution"] == "live":
        raise ConfigError("only live input may bind a live execution adapter")
    if mode == "unbound":
        if environment != {"mode": "unbound", "execution": "disabled"}:
            raise ConfigError("unbound configuration cannot select adapters")
    elif mode == "live":
        if set(environment) != {"mode", "execution", "clock"} or environment["clock"] != "host":
            raise ConfigError("live input requires host clock and no replay fields")
    else:
        if not isinstance(environment.get("recording"), str) or not environment["recording"]:
            raise ConfigError("replay requires an explicit recording path")
        if environment.get("record_format") not in ("raw-datagrams", "canonical-events", "csv", "t0md-v1"):
            raise ConfigError("unsupported replay record_format")
        if environment.get("time_basis") not in ("recorded-receive", "exchange"):
            raise ConfigError("replay requires explicit timestamp provenance")
        if environment.get("clock") != "virtual":
            raise ConfigError("replay requires a virtual clock")
        if environment["record_format"] == "t0md-v1" and environment["time_basis"] != "recorded-receive":
            raise ConfigError("t0md-v1 uses recorded receive/idle times without exchange-time substitution")


def export_legacy(config):
    validate(config)
    return _export(config)


def processing_hash(config):
    validate(config)
    # Retain the compatibility projection: field presence can affect defaults.
    return canonical_hash({"runtime": _export(config),
                           "account_ref": config["account"]["reference"],
                           "trading_day": config["daily"]["trading_day"]})


def bind_environment(config, mode, recording=None, record_format=None, time_basis=None):
    validate(config)
    result = copy.deepcopy(config)
    if mode == "live":
        if any(value is not None for value in (recording, record_format, time_basis)):
            raise ConfigError("live binding does not accept replay arguments")
        environment = {"mode": "live", "clock": "host", "execution": "disabled"}
    elif mode == "replay":
        environment = {"mode": "replay", "clock": "virtual", "execution": "disabled",
                       "recording": recording, "record_format": record_format, "time_basis": time_basis}
    else:
        raise ConfigError("bind mode must be live or replay")
    result["environment"] = environment
    validate(result)
    return result


def resolve_sse_daily(runtime, daily, day=None):
    require_object(runtime, "runtime")
    require_object(daily, "daily")
    check_json(runtime)
    check_json(daily)
    unknown = set(daily) - {"trading_day", "static_data_source_date", "static_data_hash",
                            "ins_params", "global_params", "static_position"}
    if unknown:
        raise ConfigError("unmapped SSE daily fields: " + ", ".join(sorted(unknown)))
    if runtime.get("market") != "SH":
        raise ConfigError("daily overlay is only defined for the SSE legacy loader")
    expected_day = day if day is not None else runtime.get("trading_day")
    if expected_day is None:
        raise ConfigError("SSE daily resolution requires --day or a fixed runtime trading_day")
    if daily.get("trading_day") != expected_day:
        raise ConfigError("SSE daily trading_day mismatch")
    if "static_data_hash" in daily and daily["static_data_hash"] != canonical_hash(daily.get("ins_params")):
        raise ConfigError("SSE daily static_data_hash mismatch")
    result = copy.deepcopy(runtime)
    result.pop("daily_config_path", None)
    for key in ("trading_day", "static_data_source_date", "ins_params", "global_params", "static_position"):
        if key in daily:
            result[key] = copy.deepcopy(daily[key])
    return result


def migrate_sze_system(system_path, daily_path, account_ref, component="trade", generator_path=None):
    if generator_path is None:
        generator_path = os.path.abspath(os.path.join(os.path.dirname(__file__),
            "../../deploy/sze/daily/prepare_sze_runtime.py"))
    spec = importlib.util.spec_from_file_location("legacy_sze_generator", generator_path)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    system, daily = load_json(system_path), load_json(daily_path)
    check_json(system)
    check_json(daily)
    day = daily.get("trading_day")
    date_value(day, "daily.trading_day")
    try:
        generator.validate_system(system, False)
        if len(inspect.signature(generator.validate_daily).parameters) == 3:
            daily = generator.validate_daily(daily, day, False)
        else:
            daily = generator.validate_daily(daily, day)
    except generator.ConfigError as error:
        raise ConfigError("legacy SZE inputs: " + str(error))
    if component == "trade":
        relative = "trade/config.json"
    elif re.fullmatch(r"worker:[0-9]+", component):
        relative = "workers/config_{:02d}.json".format(int(component.split(":")[1]))
    else:
        raise ConfigError("component must be trade or worker:N")
    with tempfile.TemporaryDirectory(prefix="unified-sze-config-") as output:
        try:
            generator.strategy_configs(system, daily, day, output)
        except generator.ConfigError as error:
            raise ConfigError("legacy SZE generation: " + str(error))
        path = os.path.join(output, relative)
        if not os.path.isfile(path):
            raise ConfigError("legacy generator did not produce component " + component)
        runtime = load_json(path)
    result = migrate_runtime(runtime, account_ref, day)
    result["migration"]["composition"] = {
        "kind": "sze-system-daily", "component": component,
        "system_sha256": file_hash(system_path), "daily_sha256": file_hash(daily_path),
        "generator_sha256": file_hash(generator_path),
    }
    return result


def artifact_paths(config):
    prediction = config["prediction"]
    result = {}
    for key, value in prediction.items():
        if key.endswith("_path") and isinstance(value, str) and value:
            result[key] = value
    snapshot = prediction.get("snapshot_legacy15", {})
    if isinstance(snapshot, dict) and snapshot.get("enabled"):
        for key in ("model_path", "scaler_path"):
            if not snapshot.get(key):
                raise ConfigError("enabled snapshot_legacy15 requires " + key)
            result["snapshot_legacy15." + key] = snapshot[key]
    return result


def make_lock(config, binaries):
    validate(config)
    if not binaries:
        raise ConfigError("lock requires at least one explicit --binary")
    paths = artifact_paths(config)
    paths.update({"binary:" + str(index): path for index, path in enumerate(binaries)})
    files = {}
    for name, path in sorted(paths.items()):
        files[name] = {"path": os.path.abspath(path), "sha256": file_hash(path)}
    expected = config["prediction"].get("model_sha256")
    if expected is not None and files["model_path"]["sha256"] != expected:
        raise ConfigError("model file does not match configured SHA-256")
    return {"schema_version": 1, "processing_sha256": processing_hash(config), "files": files}


def verify_lock(config, lock):
    require_object(lock, "lock")
    if lock.get("schema_version") != 1 or lock.get("processing_sha256") != processing_hash(config):
        raise ConfigError("processing configuration differs from lock")
    files = lock.get("files")
    require_object(files, "lock.files")
    for item in files.values():
        require_object(item, "lock file")
        if set(item) != {"path", "sha256"} or not isinstance(item["path"], str):
            raise ConfigError("invalid locked artifact")
    binary_names = [name for name in files if name.startswith("binary:")]
    if set(binary_names) != {"binary:" + str(index) for index in range(len(binary_names))}:
        raise ConfigError("lock binary indices must be contiguous")
    binaries = [files["binary:" + str(index)]["path"] for index in range(len(binary_names))]
    actual = make_lock(config, binaries)
    if actual != lock:
        raise ConfigError("artifact files or hashes differ from lock")
    return True


def make_run_manifest(config, artifact_lock):
    verify_lock(config, artifact_lock)
    environment = config["environment"]
    if environment["mode"] == "unbound":
        raise ConfigError("bind an environment before creating a run manifest")
    recording = None
    if environment["mode"] == "replay":
        path = os.path.abspath(environment["recording"])
        if environment["record_format"] == "t0md-v1":
            if not os.path.isdir(path):
                raise ConfigError("t0md-v1 requires a recording directory")
            names = sorted(os.listdir(path))
            expected = ["stream_{:06d}.t0md".format(index) for index in range(len(names))]
            if not names or names != expected:
                raise ConfigError("t0md-v1 segment list is empty, non-contiguous or contains unexpected files")
            recording = {"path": path, "segments": [
                {"name": name, "sha256": file_hash(os.path.join(path, name))} for name in names]}
        else:
            recording = {"path": path, "sha256": file_hash(path)}
    return {"schema_version": 1, "environment_sha256": canonical_hash(environment),
            "artifacts_lock_sha256": canonical_hash(artifact_lock), "recording": recording,
            "runtime_parity_verified": False}


def verify_run_manifest(config, artifact_lock, manifest):
    if make_run_manifest(config, artifact_lock) != manifest:
        raise ConfigError("run environment or recording differs from manifest")
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command")
    migrate = sub.add_parser("migrate", help="migrate a resolved runtime, optionally resolving SSE daily")
    migrate.add_argument("--runtime", required=True)
    migrate.add_argument("--daily")
    migrate.add_argument("--day", type=int)
    migrate.add_argument("--account-ref", required=True)
    migrate.add_argument("--output", required=True)
    sze = sub.add_parser("migrate-sze", help="use the existing system/daily generator without TD access")
    sze.add_argument("--system", required=True)
    sze.add_argument("--daily", required=True)
    sze.add_argument("--account-ref", required=True)
    sze.add_argument("--component", default="trade")
    sze.add_argument("--generator")
    sze.add_argument("--output", required=True)
    for name in ("validate", "compare", "audit-legacy", "bind", "lock", "verify-lock",
                 "lock-run", "verify-run"):
        command = sub.add_parser(name)
        command.add_argument("--config", required=True)
        if name in ("audit-legacy", "bind", "lock", "lock-run"):
            command.add_argument("--output", required=True)
        if name == "bind":
            command.add_argument("--mode", required=True, choices=("live", "replay"))
            command.add_argument("--recording")
            command.add_argument("--record-format", choices=("raw-datagrams", "canonical-events", "csv", "t0md-v1"))
            command.add_argument("--time-basis", choices=("recorded-receive", "exchange"))
        if name == "lock":
            command.add_argument("--binary", action="append", required=True)
        if name == "verify-lock":
            command.add_argument("--lock", required=True)
        if name == "compare":
            command.add_argument("--runtime", required=True)
        if name in ("lock-run", "verify-run"):
            command.add_argument("--artifacts-lock", required=True)
        if name == "verify-run":
            command.add_argument("--manifest", required=True)
    args = parser.parse_args(argv)
    if not args.command:
        parser.error("a subcommand is required")
    if args.command == "migrate":
        runtime = load_json(args.runtime)
        composition = None
        if args.daily:
            runtime = resolve_sse_daily(runtime, load_json(args.daily), args.day)
            composition = {"kind": "sse-live-daily", "live_sha256": file_hash(args.runtime),
                           "daily_sha256": file_hash(args.daily)}
        elif "daily_config_path" in runtime:
            raise ConfigError("resolve daily_config_path explicitly using --daily and a fixed trading day")
        config = migrate_runtime(runtime, args.account_ref, args.day)
        if composition:
            config["migration"]["composition"] = composition
        write_json(args.output, config)
    elif args.command == "migrate-sze":
        config = migrate_sze_system(args.system, args.daily, args.account_ref, args.component, args.generator)
        write_json(args.output, config)
    else:
        config = load_json(args.config)
        validate(config)
        if args.command == "audit-legacy":
            # This is an exact audit projection, not a runnable replay config.
            write_json(args.output, export_legacy(config))
        elif args.command == "bind":
            config = bind_environment(config, args.mode, args.recording, args.record_format, args.time_basis)
            write_json(args.output, config)
        elif args.command == "lock":
            write_json(args.output, make_lock(config, args.binary))
        elif args.command == "verify-lock":
            verify_lock(config, load_json(args.lock))
        elif args.command == "compare":
            if canonical_hash(export_legacy(config)) != canonical_hash(load_json(args.runtime)):
                raise ConfigError("legacy runtime projection differs from reference")
        elif args.command == "lock-run":
            write_json(args.output, make_run_manifest(config, load_json(args.artifacts_lock)))
        elif args.command == "verify-run":
            verify_run_manifest(config, load_json(args.artifacts_lock), load_json(args.manifest))
    print(json.dumps({"ok": True, "market": config["market"],
                      "instruments": len(config["daily"]["instruments"]),
                      "processing_sha256": processing_hash(config),
                      "environment": config["environment"]["mode"],
                      "runtime_binding_implemented": False}, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (ConfigError, ValueError, KeyError, TypeError, OSError) as error:
        raise SystemExit("unified_config_error: " + str(error))
