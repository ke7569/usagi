#!/usr/bin/env python3
"""Offline-build a full Shenzhen v0.4 shadow/recovery config from Tushare."""
from __future__ import print_function

import argparse
import csv
import datetime
import hashlib
import json
import math
import os
import tempfile
from decimal import Decimal, ROUND_HALF_UP


PREFIXES = ("000", "001", "002", "003", "300", "301")
MODEL_SHA256 = "ce5bf6378a45e9a90f1f00607cc1ced4c31a80b1d99ac0e58d28c2745f14ce6d"
WORKER_CPUS = (16, 17, 18, 19, 20, 21, 22, 23)
WORKER_STATE_CPUS = (24, 25, 26, 27, 28, 29, 30, 31)


def date_text(value):
    text = str(value or "").replace("-", "").strip()
    datetime.datetime.strptime(text, "%Y%m%d")
    return text


def atomic_text(path, text):
    directory = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(directory):
        os.makedirs(directory)
    fd, temporary = tempfile.mkstemp(prefix=".sze-v04.", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def rounded_limit(price, ratio):
    value = Decimal(str(price)) * Decimal(str(ratio))
    return float(value.quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))


def fnv1a(text):
    value = 1469598103934665603
    for byte in text.encode("ascii"):
        value ^= byte
        value = (value * 1099511628211) & 0xffffffffffffffff
    return value


def valid_number(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-date", required=True)
    parser.add_argument("--source-date", default=None)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--audit-dir", default=None,
                        help="optional archive directory for audit/universe/rejected files")
    parser.add_argument("--emit-md-config", action="store_true",
                        help="also emit the rarely-changing MD config")
    parser.add_argument("--model-path", default="/home/zane/models/mix153060_sze_v04_a3_eff60.bin")
    parser.add_argument("--token-env", default="TUSHARE_TOKEN")
    parser.add_argument("--history-calendar-days", type=int, default=45)
    args = parser.parse_args(argv)
    target = date_text(args.target_date)
    token = os.environ.get(args.token_env, "").strip()
    if not token:
        raise SystemExit("missing {}".format(args.token_env))
    import tushare as ts
    pro = ts.pro_api(token)

    start = (datetime.datetime.strptime(target, "%Y%m%d") -
             datetime.timedelta(days=args.history_calendar_days)).strftime("%Y%m%d")
    calendar = pro.trade_cal(
        exchange="SSE", start_date=start, end_date=target,
        fields="cal_date,is_open,pretrade_date",
    )
    open_dates = sorted(str(value) for value in calendar[calendar.is_open == 1].cal_date)
    source = date_text(args.source_date) if args.source_date else str(
        calendar[calendar.cal_date == target].iloc[0].pretrade_date
    )
    history_dates = [value for value in open_dates if value <= source]
    if len(history_dates) < 20:
        raise SystemExit("fewer than 20 completed trading dates before {}".format(target))

    basic = pro.stock_basic(
        exchange="SZSE", list_status="L",
        fields="ts_code,symbol,name,market,list_date",
    )
    basic = basic[basic.symbol.str[:3].isin(PREFIXES)].copy()
    basic_rows = {row["ts_code"]: row for row in basic.to_dict("records")}

    daily_by_code = {}
    for trade_date in history_dates[-20:]:
        frame = pro.daily(
            trade_date=trade_date,
            fields="ts_code,trade_date,close,pre_close,amount",
        )
        for row in frame.to_dict("records"):
            code = str(row.get("ts_code") or "")
            if code in basic_rows:
                daily_by_code.setdefault(code, []).append(row)

    free_frame = pro.daily_basic(
        trade_date=source,
        fields="ts_code,trade_date,total_share,float_share,free_share",
    )
    free_by_code = {row["ts_code"]: row for row in free_frame.to_dict("records")}
    for code in sorted(set(basic_rows) - set(free_by_code)):
        history_free = pro.daily_basic(
            ts_code=code, start_date=start, end_date=source,
            fields="ts_code,trade_date,total_share,float_share,free_share",
        )
        if history_free is not None and not history_free.empty:
            rows = sorted(history_free.to_dict("records"), key=lambda row: row["trade_date"])
            free_by_code[code] = rows[-1]
    previous_limits = pro.stk_limit(
        trade_date=source,
        fields="trade_date,ts_code,up_limit,down_limit",
    )
    previous_limit_by_code = {
        row["ts_code"]: row for row in previous_limits.to_dict("records")
    }
    target_limits = pro.stk_limit(
        trade_date=target,
        fields="trade_date,ts_code,up_limit,down_limit",
    )
    target_limit_by_code = {
        row["ts_code"]: row for row in target_limits.to_dict("records")
    }

    instruments = []
    rejected = []
    audit = []
    params = {}
    for code in sorted(basic_rows):
        meta = basic_rows[code]
        history = sorted(daily_by_code.get(code, []), key=lambda row: row["trade_date"])
        valid_history = [
            row for row in history
            if valid_number(row.get("close")) and valid_number(row.get("amount")) and
               float(row["close"]) > 0.0 and float(row["amount"]) > 0.0
        ]
        free_row = free_by_code.get(code)
        free_share = valid_number(free_row.get("free_share")) if free_row else None
        if not valid_history or not free_share or free_share <= 0.0:
            rejected.append({
                "code": code, "name": meta.get("name", ""),
                "reason": "missing_history" if not valid_history else "missing_free_share",
            })
            continue
        free_share_source_date = date_text(free_row.get("trade_date"))
        latest = valid_history[-1]
        pre_close = float(latest["close"])
        amount_rows = valid_history[-5:]
        amounts = [float(row["amount"]) * 1000.0 for row in amount_rows]
        history_amount = math.fsum(amounts) / float(len(amounts))
        closes = [float(row["close"]) for row in valid_history[-21:]]
        returns = [math.log(closes[index] / closes[index - 1])
                   for index in range(1, len(closes))
                   if closes[index] > 0.0 and closes[index - 1] > 0.0]
        history_volatility = 0.0
        if len(returns) >= 2:
            mean_return = math.fsum(returns) / len(returns)
            history_volatility = math.sqrt(
                math.fsum((value - mean_return) ** 2 for value in returns) /
                (len(returns) - 1)
            )

        limit_source = "tushare.stk_limit.target"
        limit_row = target_limit_by_code.get(code)
        upper = valid_number(limit_row.get("up_limit")) if limit_row else None
        lower = valid_number(limit_row.get("down_limit")) if limit_row else None
        if not upper or not lower or upper <= 0.0 or lower <= 0.0:
            previous = previous_limit_by_code.get(code)
            prev_upper = valid_number(previous.get("up_limit")) if previous else None
            prev_lower = valid_number(previous.get("down_limit")) if previous else None
            if prev_upper and prev_upper > 100000.0 and prev_lower and prev_lower <= 0.01:
                upper, lower = prev_upper, prev_lower
                limit_source = "previous_no_limit_band"
            else:
                name = str(meta.get("name") or "").upper()
                ratio = 0.05 if "ST" in name else (0.20 if meta.get("market") == "创业板" else 0.10)
                upper = rounded_limit(pre_close, 1.0 + ratio)
                lower = rounded_limit(pre_close, 1.0 - ratio)
                limit_source = "derived_from_board_and_name"

        short = code[:6]
        instruments.append(short)
        params[code] = {
            "Date": int(target),
            "Close": pre_close,
            "HistoryAmount": history_amount,
            "FreeShare": free_share,
            "HpUpperPrice": upper,
            "HpLowerPrice": lower,
            "HistoryVolatility20d": history_volatility,
            "static_position": 0,
            "last_position": 0,
            "cpu": WORKER_CPUS[fnv1a(code) % len(WORKER_CPUS)],
        }
        audit.append({
            "instrument": code,
            "name": meta.get("name", ""),
            "market": meta.get("market", ""),
            "history_amount": history_amount,
            "history_dates": [row["trade_date"] for row in amount_rows],
            "history_observations": len(amount_rows),
            "history_volatility_20d": history_volatility,
            "history_volatility_observations": len(returns),
            "free_share": free_share,
            "free_share_source_date": free_share_source_date,
            "pre_close_source_date": latest["trade_date"],
            "limit_source": limit_source,
        })

    config = {
        "strategy_name": "sze_recovery_all_{}".format(target),
        "market": "SZ",
        "trading_day": int(target),
        "static_data_source_date": int(source),
        "mode": "hp-shadow",
        "model_path": args.model_path,
        "mix153060_model_sha256": MODEL_SHA256,
        "sze_startup_warmup_signals": 50,
        "worker_count": len(WORKER_CPUS),
        "worker_cpus": list(WORKER_CPUS),
        "worker_state_cpus": list(WORKER_STATE_CPUS),
        "md_source_index": [],
        "td_source_index": [],
        "ins_params": params,
        "global_params": {
            "offset": 0.8, "quote_offset": 5, "bias_factor": 0.5,
            "position_limit": 0.3, "global_bias_factor": 1,
            "position_base_line": 100000.0,
        },
        "vtd": [],
        "sze_recovery_consumer": {
            "enabled": True, "trading_day": int(target), "source_id": 88,
            "journal_directory": "/home/zane/data/sze_journal_{}".format(target),
            "journal_prefix": "sze_all", "journal_segment_mb": 1024,
            "journal_max_payload_bytes": 128,
            "shm_path": "/dev/shm/sze_all_{}.events".format(target),
            "state_cpu": 7, "strategy_cpu": 8,
            "worker_count": len(WORKER_CPUS),
            "worker_cpus": list(WORKER_CPUS),
            "worker_state_cpus": list(WORKER_STATE_CPUS),
        },
        "sze_prediction_capture": {
            "enabled": True,
            "directory": "/home/zane/run_main/log/sze_all_{}".format(target),
            "prefix": "sze_all_{}".format(target),
            "output_format": "sze_log",
            "detail_instruments": ["000001.SZ"] if "000001" in instruments else [],
            "events": True, "samples": True, "capture_only": True,
            "flush_rows": 4096,
            "flush_interval_ms": 1000,
            "log_batch_bytes": 1048576,
            "log_queue_bytes": 268435456,
        },
    }
    output_dir = os.path.abspath(args.output_dir)
    archive_dir = os.path.abspath(args.audit_dir) if args.audit_dir else None
    config_path = os.path.join(output_dir, "config_sze_daily_{}.json".format(target))
    md_path = os.path.join(output_dir, "deepwin_sze_daily.json")
    main_path = os.path.join(output_dir, "main_sze_daily_{}.conf".format(target))
    if archive_dir and not os.path.isdir(archive_dir):
        os.makedirs(archive_dir)
    atomic_text(config_path, json.dumps(config, ensure_ascii=True, indent=2) + "\n")
    md_config = {
        "md": {"sze": {
            "batch": 256,
            "use_subscribe_filter": False,
            "subscribe_all": True,
            "symbols": [],
            "recoverable_pipeline": {
                "enabled": True,
                "backend": "socket",
                "trading_day": int(target),
                "journal_directory": "/home/zane/data/sze_journal_{}".format(target),
                "journal_prefix": "sze_all",
                "journal_segment_mb": 1024,
                "journal_max_payload_bytes": 128,
                "journal_min_free_gb_after_allocate": 80,
                "flush_interval_ms": 100,
                "flush_cpu": 6,
                "shm_path": "/dev/shm/sze_all_{}.events".format(target),
                "shm_capacity": 1048576,
                "shm_max_payload_bytes": 128,
                "replace_stale_shm": True,
                "unlink_shm_on_clean_shutdown": False,
                "malformed_diagnostic_path": "/home/zane/data/sze_journal_{}/sze_all_{}_malformed.bin".format(target, target),
                "malformed_diagnostic_max_records": 1000,
            },
            "channels": [{
                "multicast_ip": "239.35.81.1",
                "port": 37101,
                "iface_ip": "11.11.11.11",
                "ifname": "hqh-p1-k2",
                "bind_ip": "0.0.0.0",
                "bind_port": 37101,
                "cpu": 5,
                "rcvbuf_mb": 256,
                "busy_poll_us": 0,
                "realtime_prio": 0,
            }],
        }}}
    if args.emit_md_config:
        atomic_text(md_path, json.dumps(md_config, ensure_ascii=True, indent=2) + "\n")
    main_config = {
        "base_rid": 1200000,
        "vmd": [],
        "vtd": [],
        "vstr": [{
            "lib": "./libt0_strategy_sze.so",
            "config": "./configs/config_sze_daily_{}.json".format(target),
        }],
        "zmq": "tcp://127.0.0.1:6566",
    }
    atomic_text(main_path, json.dumps(main_config, ensure_ascii=True, indent=2) + "\n")
    if archive_dir:
        audit_path = os.path.join(archive_dir, "static_audit_{}.json".format(target))
        rejected_path = os.path.join(archive_dir, "rejected_{}.json".format(target))
        universe_path = os.path.join(archive_dir, "universe_{}.csv".format(target))
        atomic_text(audit_path, json.dumps({
            "target_date": target, "source_date": source,
            "universe_count": len(basic_rows), "accepted_count": len(instruments),
            "rejected_count": len(rejected), "history_amount_days": 5,
            "instruments": audit,
        }, ensure_ascii=True, indent=2) + "\n")
        atomic_text(rejected_path, json.dumps({
            "target_date": target, "rejected": rejected,
        }, ensure_ascii=True, indent=2) + "\n")
        rows = ["instrument_id,name,market\n"]
        for code in instruments:
            meta = basic_rows[code + ".SZ"]
            rows.append("{},{},{}\n".format(code + ".SZ", meta.get("name", ""), meta.get("market", "")))
        atomic_text(universe_path, "".join(rows))
    print("target_date={}".format(target))
    print("source_date={}".format(source))
    print("universe={}".format(len(basic_rows)))
    print("accepted={}".format(len(instruments)))
    print("rejected={}".format(len(rejected)))
    print("config={}".format(config_path))
    print("main={}".format(main_path))


if __name__ == "__main__":
    main()
