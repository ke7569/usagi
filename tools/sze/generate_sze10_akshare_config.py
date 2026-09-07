#!/usr/bin/env python3
"""Fetch reproducible Shenzhen statics with akshare and emit shadow configs."""

import argparse
import concurrent.futures
import csv
import hashlib
import json
import math
import os
import random
import re
import time
from decimal import Decimal, ROUND_HALF_UP

import akshare as ak


SZ_CODE = re.compile(r"^(000|001|002|003|300|301)[0-9]{3}$")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def limit_price(close, ratio):
    value = Decimal(str(close)) * Decimal(str(ratio))
    return float(value.quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))


def sample_stddev(values):
    if len(values) < 2:
        return 0.0
    mean = math.fsum(values) / len(values)
    return math.sqrt(math.fsum((value - mean) ** 2 for value in values) /
                     (len(values) - 1))


def fetch_history(code, start_date, end_date, retries):
    last_error = None
    for attempt in range(retries):
        try:
            frame = ak.stock_zh_a_daily(
                symbol="sz" + code,
                start_date=start_date,
                end_date=end_date,
                adjust="",
            )
            if frame is not None and not frame.empty:
                return frame
        except Exception as error:
            last_error = error
        time.sleep(0.4 * (attempt + 1))
    if last_error:
        raise last_error
    raise RuntimeError("empty history")


def select_instruments(target_date, prior_date, count, seed, retries,
                       include_st, include_suspended, workers,
                       history_start_date):
    universe = ak.stock_info_a_code_name()
    candidates = []
    for row in universe.itertuples(index=False):
        code = str(row[0]).zfill(6)
        name = str(row[1])
        if not SZ_CODE.match(code):
            continue
        if "退" in name or name.startswith(("N", "C")):
            continue
        if not include_st and "ST" in name.upper():
            continue
        candidates.append((code, name))
    random.Random(seed).shuffle(candidates)

    selected = []
    rejected = []
    start_date = history_start_date or (str(int(target_date[:4]) - 1) + target_date[4:])

    def load_candidate(candidate):
        code, name = candidate
        try:
            frame = fetch_history(code, start_date, prior_date, retries)
            frame = frame.copy()
            frame["date_text"] = frame["date"].astype(str).str.replace("-", "", regex=False)
            frame = frame[frame["date_text"] <= prior_date].sort_values("date_text")
            if len(frame) < 25:
                return None, {"code": code, "name": name,
                              "reason": "insufficient_history"}
            if frame.iloc[-1]["date_text"] != prior_date:
                if not include_suspended:
                    return None, {"code": code, "name": name,
                                  "reason": "suspended_or_missing_prior_close"}
                return None, None
            if any(float(value) <= 0.0 for value in frame.tail(3)["amount"]):
                return None, {"code": code, "name": name,
                              "reason": "missing_recent_turnover"}
            return (code, name, frame), None
        except Exception as error:
            return None, {"code": code, "name": name,
                          "reason": "history_fetch_error", "detail": str(error)}

    if workers > 1:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
            results = executor.map(load_candidate, candidates)
            for item, rejection in results:
                if item is not None:
                    selected.append(item)
                    print("selected {} {} ({}/{})".format(
                        item[0], item[1], len(selected), count))
                    if count > 0 and len(selected) == count:
                        break
                elif rejection is not None:
                    print("skip {} {}: {}".format(
                        rejection["code"], rejection["name"], rejection["reason"]))
                    rejected.append(rejection)
    else:
        for candidate in candidates:
            item, rejection = load_candidate(candidate)
            if item is not None:
                selected.append(item)
                print("selected {} {} ({}/{})".format(
                    item[0], item[1], len(selected), count))
                if count > 0 and len(selected) == count:
                    break
            elif rejection is not None:
                print("skip {} {}: {}".format(
                    rejection["code"], rejection["name"], rejection["reason"]))
                rejected.append(rejection)
    if count > 0 and len(selected) < count:
        raise RuntimeError("only selected {} eligible instruments".format(len(selected)))
    return selected, rejected


def static_for(code, frame):
    closes = [float(value) for value in frame["close"]]
    amounts = [float(value) for value in frame["amount"]]
    returns = [math.log(closes[index] / closes[index - 1])
               for index in range(max(1, len(closes) - 20), len(closes))
               if closes[index] > 0.0 and closes[index - 1] > 0.0]
    close = closes[-1]
    board_ratio = 0.20 if code.startswith(("300", "301")) else 0.10
    return {
        "Close": close,
        "HistoryAmount": math.fsum(amounts[-3:]) / 3.0,
        "HpUpperPrice": limit_price(close, 1.0 + board_ratio),
        "HpLowerPrice": limit_price(close, 1.0 - board_ratio),
        "HistoryVolatility20d": sample_stddev(returns),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--target-date", default="20260728")
    parser.add_argument("--prior-date", default="20260727")
    parser.add_argument("--count", type=int, default=10,
                        help="0 selects every eligible Shenzhen A-share")
    parser.add_argument("--seed", type=int, default=20260728)
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument("--workers", type=int, default=1,
                        help="Concurrent AkShare history requests")
    parser.add_argument("--history-start-date", default=None,
                        help="Optional YYYYMMDD history start; must provide >=25 sessions")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--model-local", required=True)
    parser.add_argument("--model-runtime", default="/home/zane/models/mix153060_sze_v04_a3_eff60.bin")
    parser.add_argument("--name", default="sze10")
    parser.add_argument("--include-st", action="store_true")
    parser.add_argument("--include-suspended", action="store_true")
    parser.add_argument("--shard-count", type=int, default=1)
    args = parser.parse_args()

    if args.count < 0 or args.shard_count < 1 or args.workers < 1:
        raise ValueError("count/workers must be non-negative/positive and shard-count must be positive")
    selected, rejected = select_instruments(
        args.target_date, args.prior_date, args.count, args.seed, args.retries,
        args.include_st, args.include_suspended, args.workers,
        args.history_start_date)
    model_hash = sha256(args.model_local)
    codes = [code for code, _, _ in selected]
    symbols = [code + ".SZ" for code in codes]
    params = {}
    amounts = []
    audit = []
    for code, name, frame in selected:
        values = static_for(code, frame)
        amounts.append(values["HistoryAmount"])
        params[code + ".SZ"] = dict({
            "static_position": 0,
            "last_position": 0,
            "Date": int(args.target_date),
            "static_data_source_date": int(args.prior_date),
        }, **values)
        audit.append(dict({"code": code, "name": name}, **values))

    journal_directory = "/home/zane/data/sze_journal_{}".format(args.target_date)
    shm_path = "/dev/shm/{}_{}.events".format(args.name, args.target_date)
    capture_directory = "/home/zane/run_main/log/sze_mix153060_{}_recovery_{}".format(
        args.name, args.target_date)

    recovery = {
        "strategy_name": "sze_mix153060_recovery_{}_{}".format(args.name, args.target_date),
        "name": "ZStrategy", "market": "SZ",
        "sz_orderbook_mode": "hp-shadow", "orderbook_mode": "hp-shadow",
        "model_path": args.model_runtime,
        "mix153060_model_sha256": model_hash,
        "md_source_index": [], "td_source_index": [], "vtd": [],
        "instrument_id": codes, "his_amt": amounts,
        "static_position": [0] * len(codes), "last_position": [0] * len(codes),
        "ins_params": params,
        "sze_recovery_consumer": {
            "enabled": True, "trading_day": int(args.target_date), "source_id": 88,
            "journal_directory": journal_directory, "journal_prefix": args.name,
            "journal_segment_mb": 1024, "journal_max_payload_bytes": 128,
            "shm_path": shm_path, "state_cpu": 7, "strategy_cpu": 8,
        },
        "sze_prediction_capture": {
            "enabled": True, "directory": capture_directory,
            "prefix": "{}_{}".format(args.name, args.target_date), "instruments": symbols,
            "output_format": "sze_log",
            "detail_instruments": ["000001.SZ"] if "000001.SZ" in symbols else [],
            "events": True, "samples": True, "capture_only": True,
            "flush_rows": 4096, "flush_interval_ms": 1000,
            "log_batch_bytes": 1048576, "log_queue_bytes": 268435456,
        },
        "global_params": {"offset": 0.8, "quote_offset": 5, "bias_factor": 0.5,
                          "position_limit": 0.3, "global_bias_factor": 1,
                          "position_base_line": 100000.0},
    }
    md_config = {
        "md": {"sze": {
            "batch": 256, "use_subscribe_filter": not args.name == "sze_all",
            "subscribe_all": args.name == "sze_all", "symbols": [] if args.name == "sze_all" else symbols,
            "recoverable_pipeline": {
                "enabled": True, "backend": "socket", "trading_day": int(args.target_date),
                "journal_directory": journal_directory, "journal_prefix": args.name,
                "journal_segment_mb": 1024, "journal_max_payload_bytes": 128,
                "journal_min_free_gb_after_allocate": 80, "flush_interval_ms": 100,
                "flush_cpu": 6, "shm_path": shm_path, "shm_capacity": 1048576,
                "shm_max_payload_bytes": 128, "replace_stale_shm": True,
                "unlink_shm_on_clean_shutdown": False,
                "malformed_diagnostic_path": journal_directory + "/" + args.name + "_" +
                    args.target_date + "_malformed.bin",
                "malformed_diagnostic_max_records": 1000,
            },
            "channels": [{"multicast_ip": "239.35.81.1", "port": 37101,
                          "iface_ip": "11.11.11.11", "ifname": "hqh-p1-k2",
                          "bind_ip": "0.0.0.0", "bind_port": 37101, "cpu": 5,
                          "rcvbuf_mb": 256, "busy_poll_us": 0, "realtime_prio": 0}],
        }}
    }
    capture_main = {"base_rid": 1000000,
                    "vmd": [{"source": 88, "lib": "./libsze_md.so", "name": "sze"}],
                    "vtd": [], "vstr": [], "zmq": "tcp://127.0.0.1:6565"}
    recovery_name = "config_sze_mix153060_recovery_{}_{}.json".format(
        args.name, args.target_date)
    recovery_main = {"base_rid": 1200000, "vmd": [], "vtd": [],
                     "vstr": [{"lib": "./libt0_strategy_sze.so",
                               "config": "./configs/" + recovery_name}],
                     "zmq": "tcp://127.0.0.1:6566"}

    os.makedirs(args.output_dir, exist_ok=True)
    write_json(os.path.join(args.output_dir, recovery_name), recovery)
    write_json(os.path.join(args.output_dir,
                            "main_sze_capture_{}_{}.conf".format(args.name, args.target_date)),
               capture_main)
    write_json(os.path.join(args.output_dir,
                            "main_sze_recovery_{}_{}.conf".format(args.name, args.target_date)),
               recovery_main)
    general = os.path.join(args.output_dir, "general_config")
    write_json(os.path.join(general,
                            "deepwin_sze_capture_{}_{}.json".format(args.name, args.target_date)),
               md_config)
    write_json(os.path.join(args.output_dir,
                            "static_audit_{}.json".format(args.target_date)),
               {"target_date": args.target_date, "source_date": args.prior_date,
                "selection_seed": args.seed, "akshare_version": ak.__version__,
                "include_st": args.include_st, "include_suspended": args.include_suspended,
                "candidate_count": len(selected) + len(rejected),
                "accepted_count": len(selected), "rejected_count": len(rejected),
                "model_sha256": model_hash, "instruments": audit})
    with open(os.path.join(args.output_dir,
                           "universe_{}.csv".format(args.target_date)),
              "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(audit[0].keys()))
        writer.writeheader()
        writer.writerows(audit)
    write_json(os.path.join(args.output_dir,
                            "rejected_{}.json".format(args.target_date)),
               {"target_date": args.target_date, "rejected": rejected})
    print("generated {} instruments in {}".format(len(codes), args.output_dir))
    print("symbols={}".format(",".join(symbols)))


if __name__ == "__main__":
    main()
