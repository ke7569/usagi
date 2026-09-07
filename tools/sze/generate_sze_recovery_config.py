#!/usr/bin/env python3
"""Generate a dated online Shenzhen journal-recovery strategy config.

This is a daily/offline configuration tool. It does not run in the market-data
or strategy hot path. The input JSON must already contain freshly generated
static parameters for the target trading day.
"""

from __future__ import print_function

import argparse
import datetime
import hashlib
import json
import os
import re


DATE_RE = re.compile(r"^\d{8}$")


def parse_date(value):
    text = str(value).replace("-", "")
    if not DATE_RE.match(text):
        raise ValueError("target date must be YYYYMMDD")
    datetime.datetime.strptime(text, "%Y%m%d")
    return text


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    parent = os.path.dirname(os.path.abspath(path))
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    with open(path, "w") as stream:
        json.dump(value, stream, indent=2, sort_keys=False)
        stream.write("\n")


def build_main_conf(path, strategy_path, library_path, base_rid):
    value = {
        "base_rid": int(base_rid),
        "vmd": [],
        "vtd": [],
        "vstr": [{"lib": library_path, "config": strategy_path}],
        "zmq": "tcp://127.0.0.1:6566",
    }
    write_json(path, value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-json", required=True)
    parser.add_argument("--target-date", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--output-main-conf")
    parser.add_argument("--journal-directory", required=True)
    parser.add_argument("--journal-prefix", default="000001")
    parser.add_argument("--shm-path", required=True)
    parser.add_argument("--capture-directory", required=True)
    parser.add_argument("--capture-prefix")
    parser.add_argument("--strategy-library", default="./libt0_strategy_sze.so")
    parser.add_argument("--base-rid", type=int, default=1200000)
    parser.add_argument("--state-cpu", type=int, default=7)
    parser.add_argument("--strategy-cpu", type=int, default=8)
    args = parser.parse_args()

    target_date = parse_date(args.target_date)
    with open(args.input_json, "r") as stream:
        config = json.load(stream)
    if config.get("market") != "SZ":
        raise ValueError("input strategy config must use market=SZ")
    mode = config.get("sz_orderbook_mode", config.get("mode", config.get("orderbook_mode")))
    if mode not in ("hp-shadow", "hp_shadow"):
        raise ValueError("online recovery training requires hp-shadow mode")
    if config.get("md_source_index", []) != [] or config.get("td_source_index", []) != []:
        raise ValueError("journal recovery config requires empty md_source_index and td_source_index")

    model_path = config.get("model_path")
    if not model_path or not os.path.isfile(model_path):
        raise ValueError("model_path does not exist: {}".format(model_path))
    model_hash = sha256(model_path)

    params = config.get("ins_params") or {}
    instruments = config.get("instrument_id") or [
        str(code).split(".")[0] for code in sorted(params)
    ]
    if not instruments or not params:
        raise ValueError("input config must contain non-empty ins_params")
    for values in params.values():
        if isinstance(values, dict):
            for audit_key in ("free_share_unit", "free_share_source", "free_share_source_date"):
                values.pop(audit_key, None)
    for raw_code in instruments:
        code = str(raw_code)
        key = code if "." in code else code + ".SZ"
        if key not in params and code in params:
            key = code
        if key not in params:
            raise ValueError("missing ins_params for {}".format(key))
        date_value = params[key].get("Date")
        if int(date_value) != int(target_date):
            raise ValueError(
                "static Date mismatch for {}: {} != {}".format(
                    key, date_value, target_date))

    config["strategy_name"] = "sze_mix153060_recovery_{}".format(target_date)
    config.pop("name", None)
    config.pop("sz_orderbook_mode", None)
    config.pop("orderbook_mode", None)
    config["mode"] = "hp-shadow"
    config["model_path"] = model_path
    config["mix153060_model_sha256"] = model_hash
    config["md_source_index"] = []
    config["td_source_index"] = []
    config["vtd"] = []
    for redundant in ("instrument_id", "his_amt", "static_position", "last_position"):
        config.pop(redundant, None)
    config.pop("allow_invalid_replay_for_analysis", None)

    config["sze_recovery_consumer"] = {
        "enabled": True,
        "trading_day": int(target_date),
        "source_id": 88,
        "journal_directory": args.journal_directory,
        "journal_prefix": args.journal_prefix,
        "journal_segment_mb": 1024,
        "journal_max_payload_bytes": 128,
        "journal_min_free_gb_after_allocate": 80,
        "shm_path": args.shm_path,
        "state_cpu": args.state_cpu,
        "strategy_cpu": args.strategy_cpu,
    }
    config["sze_prediction_capture"] = {
        "enabled": True,
        "directory": args.capture_directory,
        "prefix": args.capture_prefix or "000001_{}".format(target_date),
        "output_format": "sze_log",
        "detail": True,
        "events": True,
        "samples": True,
        "capture_only": True,
        "flush_rows": 4096,
        "flush_interval_ms": 1000,
        "log_batch_bytes": 1048576,
        "log_queue_bytes": 268435456,
    }
    write_json(args.output_json, config)
    if args.output_main_conf:
        build_main_conf(
            args.output_main_conf,
            "./configs/{}".format(os.path.basename(args.output_json)),
            args.strategy_library,
            args.base_rid,
        )
    print("config={}".format(os.path.abspath(args.output_json)))
    if args.output_main_conf:
        print("main_conf={}".format(os.path.abspath(args.output_main_conf)))
    print("model_sha256={}".format(model_hash))


if __name__ == "__main__":
    main()
