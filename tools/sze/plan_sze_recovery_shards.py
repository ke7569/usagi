#!/usr/bin/env python3
"""Split an all-market Shenzhen recovery config into stable shadow shards."""

import argparse
import copy
import json
import os


def write_json(path, value):
    parent = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(parent):
        os.makedirs(parent)
    with open(path, "w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def fnv1a(text):
    value = 1469598103934665603
    for byte in text.encode("ascii"):
        value ^= byte
        value = (value * 1099511628211) & 0xffffffffffffffff
    return value


def normalized_symbol(code):
    text = str(code)
    return text if "." in text else text + ".SZ"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-json", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--shards", type=int, default=8)
    parser.add_argument("--state-cpu-base", type=int, default=16)
    parser.add_argument("--capture-directory", required=True)
    parser.add_argument("--runtime-config-root", default="/home/zane/configs")
    parser.add_argument("--dated-filenames", action="store_true")
    args = parser.parse_args()
    if args.shards < 1 or args.shards > 16:
        raise ValueError("shards must be in [1,16]")
    with open(args.input_json, encoding="utf-8") as stream:
        base = json.load(stream)
    params = base.get("ins_params") or {}
    codes = list(base.get("instrument_id") or [
        str(key).split(".")[0] for key in sorted(params)
    ])
    amounts = list(base.get("his_amt") or [])
    if not amounts:
        for code in codes:
            symbol = normalized_symbol(code)
            if symbol not in params or "HistoryAmount" not in params[symbol]:
                raise ValueError("missing ins_params.HistoryAmount for " + symbol)
            amounts.append(params[symbol]["HistoryAmount"])
    if not codes or len(codes) != len(amounts):
        raise ValueError("configuration must contain a non-empty instrument universe")
    cpu_values = []
    for code in codes:
        symbol = normalized_symbol(code)
        value = params[symbol].get("cpu")
        if isinstance(value, int) and value >= 0:
            cpu_values.append(value)
    explicit_cpus = sorted(set(cpu_values)) if len(cpu_values) == len(codes) else []
    use_explicit_cpus = len(explicit_cpus) == args.shards
    if use_explicit_cpus:
        cpu_to_shard = {cpu: index for index, cpu in enumerate(explicit_cpus)}

    groups = [[] for _ in range(args.shards)]
    for index, code in enumerate(codes):
        symbol = normalized_symbol(code)
        if symbol not in params:
            raise ValueError("missing ins_params for " + symbol)
        if use_explicit_cpus:
            shard_id = cpu_to_shard[params[symbol]["cpu"]]
        else:
            shard_id = fnv1a(symbol) % args.shards
        groups[shard_id].append((str(code), amounts[index], symbol))

    manifest = {"algorithm": ("ins_params.cpu" if use_explicit_cpus else
                               "fnv1a64(normalized_symbol)%shards"),
                "shards": args.shards, "source_config": os.path.basename(args.input_json),
                "assignments": []}
    for shard_id, entries in enumerate(groups):
        config = copy.deepcopy(base)
        config["strategy_name"] = "{}_shard{:02d}".format(
            base.get("strategy_name", "sze_recovery"), shard_id)
        config["instrument_id"] = [entry[0] for entry in entries]
        config["his_amt"] = [entry[1] for entry in entries]
        config["static_position"] = [0] * len(entries)
        config["last_position"] = [0] * len(entries)
        config["ins_params"] = {entry[2]: params[entry[2]] for entry in entries}
        consumer = config["sze_recovery_consumer"]
        consumer["shard_id"] = shard_id
        consumer["shard_count"] = args.shards
        consumer["health_state_enabled"] = True
        consumer["health_state_path"] = "{}.shard.{}.health".format(
            consumer["shm_path"], shard_id)
        if use_explicit_cpus:
            consumer["strategy_cpu"] = explicit_cpus[shard_id]
            consumer["state_cpu"] = args.state_cpu_base + args.shards + shard_id
        else:
            consumer["state_cpu"] = args.state_cpu_base + shard_id
            consumer["strategy_cpu"] = args.state_cpu_base + args.shards + shard_id
        capture = config.get("sze_prediction_capture")
        if not isinstance(capture, dict):
            capture = config["mix153060_capture"]
        capture["directory"] = os.path.join(args.capture_directory,
                                             "shard_{:02d}".format(shard_id))
        capture["prefix"] = "shard_{:02d}".format(shard_id)
        capture["instruments"] = [entry[2] for entry in entries]
        for redundant in ("instrument_id", "his_amt", "static_position", "last_position"):
            config.pop(redundant, None)
        trading_day = config["sze_recovery_consumer"]["trading_day"]
        suffix = "_{}".format(trading_day) if args.dated_filenames else ""
        name = "config_sze_recovery_shard_{:02d}{}.json".format(shard_id, suffix)
        write_json(os.path.join(args.output_dir, name), config)
        main_name = "main_sze_recovery_shard_{:02d}{}.conf".format(
            shard_id, suffix)
        write_json(os.path.join(
            args.output_dir, main_name),
            {"base_rid": 1200100 + shard_id, "vmd": [], "vtd": [],
             "vstr": [{"lib": "./libt0_strategy_sze.so",
                       "config": os.path.join(args.runtime_config_root,
                                               "shards_{}".format(trading_day),
                                               name)}],
             "zmq": "tcp://127.0.0.1:{}".format(6570 + shard_id)})
        manifest["assignments"].append({"shard": shard_id,
                                        "config": name,
                                        "state_cpu": consumer["state_cpu"],
                                        "strategy_cpu": consumer["strategy_cpu"],
                                        "symbol_count": len(entries),
                                        "symbols": [entry[2] for entry in entries]})
    manifest_name = ("sze_shard_manifest_{}.json".format(trading_day)
                     if args.dated_filenames else "sze_shard_manifest.json")
    write_json(os.path.join(args.output_dir, manifest_name), manifest)
    print("planned {} symbols across {} shards".format(len(codes), args.shards))


if __name__ == "__main__":
    main()
