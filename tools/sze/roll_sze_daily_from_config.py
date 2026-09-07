#!/usr/bin/env python3
"""Roll a validated Shenzhen strategy config when the data provider is unavailable."""
import argparse
import copy
import json
import re
from pathlib import Path

WORKER_CPUS = tuple(range(16, 24))
WORKER_STATE_CPUS = tuple(range(24, 32))

def fnv1a(value):
    h = 2166136261
    for b in value.encode("ascii"):
        h = ((h ^ b) * 16777619) & 0xffffffff
    return h


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--target-date", required=True)
    p.add_argument("--source-date", required=True)
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--model-path", default=None)
    a = p.parse_args()
    target = str(a.target_date)
    source = int(a.source_date)
    d = copy.deepcopy(json.loads(a.input.read_text()))
    d["strategy_name"] = "sze_recovery_all_{}".format(target)
    d["trading_day"] = int(target)
    d["static_data_source_date"] = source
    d.setdefault("sze_startup_warmup_signals", 50)
    d["worker_count"] = len(WORKER_CPUS)
    d["worker_cpus"] = list(WORKER_CPUS)
    d["worker_state_cpus"] = list(WORKER_STATE_CPUS)
    if a.model_path:
        d["model_path"] = a.model_path
    for code, params in d.get("ins_params", {}).items():
        params["Date"] = int(target)
        params["cpu"] = WORKER_CPUS[fnv1a(code) % len(WORKER_CPUS)]
    r = d.get("sze_recovery_consumer", {})
    r["trading_day"] = int(target)
    r["journal_directory"] = "/home/zane/data/sze_journal_{}".format(target)
    r["shm_path"] = "/dev/shm/sze_all_{}.events".format(target)
    r["journal_segment_mb"] = 1024
    r["journal_min_free_gb_after_allocate"] = 80
    r.setdefault("worker_count", len(WORKER_CPUS))
    r["worker_cpus"] = list(WORKER_CPUS)
    r["worker_state_cpus"] = list(WORKER_STATE_CPUS)
    c = d.get("sze_prediction_capture", {})
    c["directory"] = "/home/zane/run_main/log/sze_all_{}".format(target)
    c["prefix"] = "sze_all_{}".format(target)
    out = a.output_dir
    out.mkdir(parents=True, exist_ok=True)
    cfg = out / "config_sze_daily_{}.json".format(target)
    cfg.write_text(json.dumps(d, ensure_ascii=True, indent=2) + "\n")
    main = {
        "base_rid": 1200000, "vmd": [], "vtd": [],
        "vstr": [{"lib": "./libt0_strategy_sze.so",
                  "config": "./configs/{}".format(cfg.name)}],
        "zmq": "tcp://127.0.0.1:6566",
    }
    (out / "main_sze_daily_{}.conf".format(target)).write_text(
        json.dumps(main, ensure_ascii=True, indent=2) + "\n")
    print("rolled={} source_date={} instruments={} source_config={}".format(
        cfg, source, len(d.get("ins_params", {})), a.input))


if __name__ == "__main__":
    main()
