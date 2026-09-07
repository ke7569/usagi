#!/usr/bin/env python3
"""Validate fixed/daily SZE inputs and create ephemeral runtime configs."""

import argparse
import copy
import hashlib
import json
import os
import re
import shutil
import stat
import tempfile


SYMBOL_RE = re.compile(r"^[0-9]{6}\.SZ$")
DAILY_KEYS = {"trading_day", "static_data_source_date", "static_data_hash", "ins_params"}


class ConfigError(Exception):
    pass


def load_json(path):
    duplicates = []

    def pairs_hook(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                duplicates.append(key)
            result[key] = value
        return result

    with open(path, encoding="utf-8") as stream:
        value = json.load(stream, object_pairs_hook=pairs_hook)
    if duplicates:
        raise ConfigError("duplicate JSON keys in {}: {}".format(
            path, ",".join(sorted(set(duplicates)))))
    return value


def write_json(path, value, mode=0o644):
    parent = os.path.dirname(path)
    if not os.path.isdir(parent):
        os.makedirs(parent)
    fd, temporary = tempfile.mkstemp(prefix=".sze-config-", dir=parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def canonical_hash(value):
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def fnv1a(text):
    value = 1469598103934665603
    for byte in text.encode("ascii"):
        value ^= byte
        value = (value * 1099511628211) & 0xffffffffffffffff
    return value


def require(mapping, key, context):
    if key not in mapping:
        raise ConfigError("missing {}.{}".format(context, key))
    return mapping[key]


def validate_cpu_list(values, count, name):
    if not isinstance(values, list) or len(values) != count:
        raise ConfigError("{} must contain {} CPUs".format(name, count))
    cpus = [int(value) for value in values]
    if len(set(cpus)) != count or min(cpus) < 0:
        raise ConfigError("{} must contain distinct non-negative CPUs".format(name))
    return cpus


def validate_system(system, validate_files):
    if int(require(system, "schema_version", "system")) != 1:
        raise ConfigError("unsupported system schema_version")
    if require(system, "market", "system") != "SZ":
        raise ConfigError("system.market must be SZ")
    paths = require(system, "paths", "system")
    model = require(system, "model", "system")
    md = require(system, "market_data", "system")
    recovery = require(system, "recovery", "system")
    prediction = require(system, "prediction", "system")
    trade = require(system, "trade", "system")
    cpus = require(system, "cpu_affinity", "system")
    sessions = require(system, "market_schedule", "system")

    shard_count = int(require(recovery, "shard_count", "recovery"))
    if shard_count < 1 or shard_count > 32:
        raise ConfigError("recovery.shard_count must be in [1,32]")
    strategy_cpus = validate_cpu_list(
        require(cpus, "recovery_strategy_cpus", "cpu_affinity"),
        shard_count, "cpu_affinity.recovery_strategy_cpus")
    state_cpus = validate_cpu_list(
        require(cpus, "recovery_state_cpus", "cpu_affinity"),
        shard_count, "cpu_affinity.recovery_state_cpus")
    reserved = strategy_cpus + state_cpus + [
        int(require(cpus, "capture_receive_cpu", "cpu_affinity")),
        int(require(cpus, "journal_flush_cpu", "cpu_affinity")),
        int(require(cpus, "prediction_strategy_cpu", "cpu_affinity")),
        int(require(cpus, "prediction_state_cpu", "cpu_affinity")),
        int(require(cpus, "td_receive_cpu", "cpu_affinity")),
        int(require(cpus, "td_send_cpu", "cpu_affinity")),
    ]
    if "snapshot_receive_cpu" in cpus:
        reserved.append(int(cpus["snapshot_receive_cpu"]))
    if len(set(reserved)) != len(reserved):
        raise ConfigError("hot role CPU assignments must be disjoint")

    if int(require(recovery, "journal_segment_mb", "recovery")) != 1024:
        raise ConfigError("recovery.journal_segment_mb must be 1024")
    if int(require(recovery, "journal_min_free_gb_after_allocate", "recovery")) < 80:
        raise ConfigError("journal reserve must be at least 80 GiB")
    if int(require(recovery, "journal_max_payload_bytes", "recovery")) < 128:
        raise ConfigError("journal payload capacity must be at least 128")
    if int(require(md, "source_id", "market_data")) != 88:
        raise ConfigError("market_data.source_id must be 88")
    for key in ("capture_start", "call_auction_start", "call_auction_end",
                "continuous_auction_start", "stop"):
        require(sessions, key, "market_schedule")
    require(prediction, "global_params", "prediction")
    require(trade, "global_params", "trade")
    require(trade, "order_routing", "trade")
    require(paths, "runtime_root", "paths")
    require(paths, "journal_directory_pattern", "paths")
    require(paths, "shm_path_pattern", "paths")

    if validate_files:
        model_path = require(model, "path", "model")
        expected = require(model, "sha256", "model").lower()
        if not os.path.isfile(model_path):
            raise ConfigError("model does not exist: {}".format(model_path))
        actual = file_sha256(model_path)
        if actual != expected:
            raise ConfigError("model SHA256 mismatch: {} != {}".format(actual, expected))
    return shard_count, strategy_cpus, state_cpus


def validate_daily(daily, day):
    keys = set(daily)
    if keys != DAILY_KEYS:
        extra = sorted(keys - DAILY_KEYS)
        missing = sorted(DAILY_KEYS - keys)
        raise ConfigError("daily config must contain only {}; extra={} missing={}".format(
            sorted(DAILY_KEYS), extra, missing))
    if int(daily["trading_day"]) != day:
        raise ConfigError("daily trading_day {} does not match {}".format(
            daily["trading_day"], day))
    params = daily["ins_params"]
    if not isinstance(params, dict) or not params:
        raise ConfigError("daily.ins_params must be a non-empty object")
    for symbol, item in params.items():
        if not SYMBOL_RE.match(symbol):
            raise ConfigError("invalid Shenzhen symbol: {}".format(symbol))
        if not isinstance(item, dict):
            raise ConfigError("ins_params.{} must be an object".format(symbol))
        for key in ("Close", "HistoryAmount", "FreeShare", "HpUpperPrice",
                    "HpLowerPrice", "HistoryVolatility20d", "static_position"):
            require(item, key, "ins_params.{}".format(symbol))
        for key in ("cpu", "last_position"):
            if key in item:
                raise ConfigError("daily ins_params must not contain operational field {}.{}".format(
                    symbol, key))
        if "Date" in item and int(item["Date"]) != day:
            raise ConfigError("ins_params.{}.Date does not match {}".format(symbol, day))
        if int(item["static_position"]) < 0:
            raise ConfigError("negative static_position for {}".format(symbol))
    actual_hash = canonical_hash(params)
    expected_hash = str(daily["static_data_hash"]).lower()
    if actual_hash != expected_hash:
        raise ConfigError("static_data_hash mismatch: {} != {}".format(
            actual_hash, expected_hash))
    return daily


def day_path(pattern, day):
    return str(pattern).replace("{trading_day}", str(day))


def strategy_base(system, daily, day, capture_only):
    model = system["model"]
    recovery = system["recovery"]
    prediction = system["prediction"]
    paths = system["paths"]
    params = copy.deepcopy(daily["ins_params"])
    for item in params.values():
        item["Date"] = day
        item["last_position"] = 0
        item.pop("cpu", None)
    journal = day_path(paths["journal_directory_pattern"], day)
    shm = day_path(paths["shm_path_pattern"], day)
    output = day_path(prediction["output_directory_pattern"], day)
    return {
        "strategy_name": "sze_recovery_all_{}".format(day),
        "market": "SZ",
        "mode": prediction.get("recovery_mode", "hp-shadow"),
        "model_path": model["path"],
        "mix153060_model_sha256": model["sha256"],
        "md_source_index": [],
        "td_source_index": [],
        "ins_params": params,
        "global_params": copy.deepcopy(prediction["global_params"]),
        "vtd": [],
        "trading_day": day,
        "static_data_source_date": daily["static_data_source_date"],
        "sze_startup_warmup_signals": int(prediction.get("startup_warmup_signals", 50)),
        "sze_recovery_consumer": {
            "enabled": True,
            "trading_day": day,
            "source_id": int(system["market_data"]["source_id"]),
            "journal_directory": journal,
            "journal_prefix": recovery["journal_prefix"],
            "journal_segment_mb": int(recovery["journal_segment_mb"]),
            "journal_max_payload_bytes": int(recovery["journal_max_payload_bytes"]),
            "shm_path": shm,
        },
        "sze_prediction_capture": {
            "enabled": True,
            "directory": output,
            "prefix": "sze_all_{}".format(day),
            "output_format": prediction.get("output_format", "sze_log"),
            "detail_instruments": copy.deepcopy(prediction.get("detail_instruments", [])),
            "events": True,
            "samples": True,
            "capture_only": bool(capture_only),
            "flush_rows": int(prediction.get("flush_rows", 4096)),
            "flush_interval_ms": int(prediction.get("flush_interval_ms", 1000)),
            "log_batch_bytes": int(prediction.get("log_batch_bytes", 1048576)),
            "log_queue_bytes": int(prediction.get("log_queue_bytes", 268435456)),
        },
    }


def capture_configs(system, day, output_dir):
    md = copy.deepcopy(system["market_data"])
    recovery = system["recovery"]
    paths = system["paths"]
    md.pop("source_id", None)
    md["channels"][0]["cpu"] = int(system["cpu_affinity"]["capture_receive_cpu"])
    md["recoverable_pipeline"] = {
        "enabled": True,
        "backend": recovery.get("backend", "socket"),
        "trading_day": day,
        "journal_directory": day_path(paths["journal_directory_pattern"], day),
        "journal_prefix": recovery["journal_prefix"],
        "journal_segment_mb": int(recovery["journal_segment_mb"]),
        "journal_max_payload_bytes": int(recovery["journal_max_payload_bytes"]),
        "journal_min_free_gb_after_allocate": int(
            recovery["journal_min_free_gb_after_allocate"]),
        "flush_interval_ms": int(recovery.get("flush_interval_ms", 100)),
        "flush_cpu": int(system["cpu_affinity"]["journal_flush_cpu"]),
        "shm_path": day_path(paths["shm_path_pattern"], day),
        "shm_capacity": int(recovery.get("shm_capacity", 1048576)),
        "shm_max_payload_bytes": int(recovery["journal_max_payload_bytes"]),
        "replace_stale_shm": True,
        "unlink_shm_on_clean_shutdown": False,
        "malformed_diagnostic_path": os.path.join(
            day_path(paths["journal_directory_pattern"], day),
            "sze_all_{}_malformed.bin".format(day)),
        "malformed_diagnostic_max_records": int(
            recovery.get("malformed_diagnostic_max_records", 1000)),
    }
    write_json(os.path.join(output_dir, "deepwin.json"), {"md": {"sze": md}})
    write_json(os.path.join(output_dir, "main.conf"), {
        "base_rid": 1000000,
        "vmd": [{"source": int(system["market_data"]["source_id"]),
                 "lib": os.path.join(system["paths"]["run_main"], "libsze_md.so"),
                 "name": "sze"}],
        "vtd": [], "vstr": [], "zmq": "tcp://127.0.0.1:6565",
    })


def strategy_configs(system, daily, day, output_dir):
    shard_count, strategy_cpus, state_cpus = validate_system(system, False)
    base = strategy_base(system, daily, day, True)
    groups = [[] for _ in range(shard_count)]
    for symbol in sorted(base["ins_params"]):
        groups[fnv1a(symbol) % shard_count].append(symbol)
    workers_dir = os.path.join(output_dir, "workers")
    manifest = []
    for shard, symbols in enumerate(groups):
        config = copy.deepcopy(base)
        config["strategy_name"] = "sze_recovery_{}_shard{:02d}".format(day, shard)
        config["ins_params"] = {symbol: base["ins_params"][symbol] for symbol in symbols}
        for item in config["ins_params"].values():
            item["cpu"] = strategy_cpus[shard]
        consumer = config["sze_recovery_consumer"]
        consumer.update({
            "shard_id": shard, "shard_count": shard_count,
            "state_cpu": state_cpus[shard], "strategy_cpu": strategy_cpus[shard],
        })
        capture = config["sze_prediction_capture"]
        capture["directory"] = os.path.join(capture["directory"],
                                             "shard_{:02d}".format(shard))
        capture["prefix"] = "shard_{:02d}".format(shard)
        capture["instruments"] = symbols
        config_path = os.path.join(workers_dir, "config_{:02d}.json".format(shard))
        main_path = os.path.join(workers_dir, "main_{:02d}.conf".format(shard))
        write_json(config_path, config)
        write_json(main_path, {
            "base_rid": 1200100 + shard,
            "vmd": [], "vtd": [],
            "vstr": [{"lib": os.path.join(system["paths"]["run_main"],
                                             "libt0_strategy_sze.so"),
                      "config": config_path}],
            "zmq": "tcp://127.0.0.1:{}".format(6570 + shard),
        })
        manifest.append({"shard": shard, "symbol_count": len(symbols),
                         "strategy_cpu": strategy_cpus[shard],
                         "state_cpu": state_cpus[shard]})
    trade_symbols = sorted(symbol for symbol, item in base["ins_params"].items()
                           if int(item.get("static_position", 0)) != 0)
    if system["trade"].get("enabled", False) and trade_symbols:
        trade = strategy_base(system, daily, day, False)
        trade["strategy_name"] = "sze_realtime_trade_{}".format(day)
        trade["mode"] = system["trade"].get("mode", "hp-realtime")
        trade["ins_params"] = {symbol: trade["ins_params"][symbol]
                               for symbol in trade_symbols}
        trade["global_params"] = copy.deepcopy(system["trade"]["global_params"])
        trade["td_source_index"] = [int(system["trade"]["td_source_id"])]
        trade["sze_order_routing"] = copy.deepcopy(system["trade"]["order_routing"])
        trade["sze_recovery_consumer"].update({
            "state_cpu": int(system["cpu_affinity"]["prediction_state_cpu"]),
            "strategy_cpu": int(system["cpu_affinity"]["prediction_strategy_cpu"]),
            "trading_enabled": True,
        })
        trade["sze_prediction_capture"]["directory"] = day_path(
            system["trade"]["output_directory_pattern"], day)
        trade["sze_prediction_capture"]["prefix"] = "sze_trade_{}".format(day)
        trade["sze_prediction_capture"]["detail_instruments"] = trade_symbols
        trade_dir = os.path.join(output_dir, "trade")
        trade_config = os.path.join(trade_dir, "config.json")
        write_json(trade_config, trade)
        write_json(os.path.join(trade_dir, "main.conf"), {
            "base_rid": 1710000,
            "vmd": [],
            "vtd": [{"source": int(system["trade"]["td_source_id"]),
                     "lib": os.path.join(system["paths"]["run_main"], "libsze_td.so"),
                     "name": "sze_td"}],
            "vstr": [{"lib": os.path.join(system["paths"]["run_main"],
                                             "libt0_strategy_sze.so"),
                      "config": trade_config}],
            "zmq": "tcp://127.0.0.1:6601",
        })
    write_json(os.path.join(output_dir, "manifest.json"), {
        "trading_day": day,
        "static_data_hash": daily["static_data_hash"],
        "model_sha256": system["model"]["sha256"],
        "shards": manifest,
        "trade_symbols": trade_symbols,
        "credentials_path": system["trade"].get("credentials_path", ""),
    })


def validate_credentials(system):
    if not system["trade"].get("enabled", False):
        return
    path = require(system["trade"], "credentials_path", "trade")
    info = os.stat(path)
    if stat.S_IMODE(info.st_mode) & 0o077:
        raise ConfigError("TD credentials must have mode 0600: {}".format(path))


def replace_component(component_dir, builder):
    parent = os.path.dirname(component_dir)
    if not os.path.isdir(parent):
        os.makedirs(parent)
    temporary = tempfile.mkdtemp(prefix=".{}-".format(os.path.basename(component_dir)),
                                 dir=parent)
    try:
        builder(temporary)
        old = component_dir + ".old"
        if os.path.isdir(old):
            shutil.rmtree(old)
        if os.path.isdir(component_dir):
            os.rename(component_dir, old)
        os.rename(temporary, component_dir)
        if os.path.isdir(old):
            shutil.rmtree(old)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def update_current(runtime_root, day_dir):
    current = os.path.join(runtime_root, "current")
    temporary = current + ".new"
    try:
        os.unlink(temporary)
    except OSError:
        pass
    os.symlink(day_dir, temporary)
    os.replace(temporary, current)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("component", choices=("capture", "strategy", "all",
                                               "validate-system", "validate",
                                               "validate-trade"))
    parser.add_argument("--system", required=True)
    parser.add_argument("--daily")
    parser.add_argument("--day", required=True, type=int)
    parser.add_argument("--runtime-root")
    parser.add_argument("--skip-file-validation", action="store_true")
    args = parser.parse_args()

    system = load_json(args.system)
    validate_files = (not args.skip_file_validation and
                      args.component not in ("capture", "validate-system"))
    validate_system(system, validate_files)
    runtime_root = args.runtime_root or system["paths"]["runtime_root"]
    day_dir = os.path.join(runtime_root, str(args.day))
    daily = None
    if args.component in ("strategy", "all", "validate", "validate-trade"):
        if not args.daily:
            raise ConfigError("--daily is required for {}".format(args.component))
        daily = validate_daily(load_json(args.daily), args.day)
    if args.component == "validate-system":
        print(json.dumps({"ok": True, "market": system["market"],
                          "daily_config_required": False},
                         sort_keys=True, separators=(",", ":")))
        return
    if args.component == "validate":
        print(json.dumps({"ok": True, "trading_day": args.day,
                          "instruments": len(daily["ins_params"]),
                          "trade_instruments": sum(
                              int(item.get("static_position", 0)) != 0
                              for item in daily["ins_params"].values())},
                         sort_keys=True, separators=(",", ":")))
        return
    if args.component == "validate-trade":
        validate_credentials(system)
        trade_count = sum(int(item.get("static_position", 0)) != 0
                          for item in daily["ins_params"].values())
        if system["trade"].get("enabled", False) and trade_count == 0:
            raise ConfigError("trade enabled but daily config has no nonzero static_position")
        print(json.dumps({"ok": True, "trading_day": args.day,
                          "trade_instruments": trade_count},
                         sort_keys=True, separators=(",", ":")))
        return
    if args.component in ("capture", "all"):
        replace_component(os.path.join(day_dir, "capture"),
                          lambda path: capture_configs(system, args.day, path))
    if args.component in ("strategy", "all"):
        replace_component(os.path.join(day_dir, "strategy"),
                          lambda path: strategy_configs(system, daily, args.day, path))
    update_current(runtime_root, day_dir)
    print(json.dumps({"ok": True, "component": args.component,
                      "trading_day": args.day, "runtime": day_dir},
                     sort_keys=True, separators=(",", ":")))


if __name__ == "__main__":
    try:
        main()
    except (ConfigError, KeyError, OSError, ValueError) as error:
        raise SystemExit("sze_config_error: {}".format(error))
