#!/usr/bin/env python3
"""Fetch Shenzhen free-float shares for a target trading date."""
from __future__ import print_function

import argparse
import csv
import datetime
import math
import os
import sys
import tempfile


def ascii_error(value):
    return str(value).encode("ascii", "replace").decode("ascii")


def parse_date(value, name):
    text = str(value or "").replace("-", "").strip()
    try:
        datetime.datetime.strptime(text, "%Y%m%d")
    except ValueError:
        raise ValueError("{} must be YYYYMMDD: {}".format(name, value))
    return text


def atomic_write(path, rows):
    directory = os.path.dirname(os.path.abspath(path)) or "."
    if not os.path.isdir(directory):
        os.makedirs(directory)
    fd, temporary = tempfile.mkstemp(prefix=".free_share.", dir=directory, text=True)
    try:
        with os.fdopen(fd, "w", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=["date", "code", "free_share", "unit", "source", "source_date"],
            )
            writer.writeheader()
            writer.writerows(rows)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-date", default=None, help="Tushare trade_date YYYYMMDD")
    parser.add_argument("--target-date", required=True, help="Strategy target date YYYYMMDD")
    parser.add_argument("--output", required=True, help="Normalized FreeShare CSV path")
    parser.add_argument("--token-env", default="TUSHARE_TOKEN")
    args = parser.parse_args(argv)
    target_date = parse_date(args.target_date, "target-date")
    token = os.environ.get(args.token_env, "").strip()
    if not token:
        raise SystemExit("missing {} environment variable".format(args.token_env))
    try:
        import tushare as ts
    except ImportError:
        raise SystemExit("tushare is not installed in the configured Python environment")
    pro = ts.pro_api(token)
    if args.source_date:
        source_date = parse_date(args.source_date, "source-date")
    else:
        try:
            calendar = pro.trade_cal(
                exchange="SSE", start_date=target_date, end_date=target_date,
                fields="cal_date,is_open,pretrade_date",
            )
        except Exception as exc:
            raise SystemExit("Tushare trade_cal failed: {}".format(ascii_error(exc)))
        if calendar is None or calendar.empty or "pretrade_date" not in calendar.columns:
            raise SystemExit("Tushare trade_cal returned no previous trading date")
        source_date = parse_date(calendar.iloc[0]["pretrade_date"], "pretrade_date")
    try:
        frame = pro.daily_basic(
            trade_date=source_date,
            fields="ts_code,trade_date,total_share,float_share,free_share",
        )
    except Exception as exc:
        raise SystemExit("Tushare daily_basic failed: {}".format(ascii_error(exc)))
    if frame is None or frame.empty:
        raise SystemExit("Tushare daily_basic returned no rows for {}".format(source_date))
    rows, seen = [], set()
    for record in frame.to_dict("records"):
        code = str(record.get("ts_code") or "").strip().upper()
        if not code.endswith(".SZ") or code[:3] not in ("000", "001", "002", "003", "300", "301"):
            continue
        try:
            value = float(record.get("free_share"))
        except (TypeError, ValueError):
            continue
        if not math.isfinite(value) or value <= 0.0:
            continue
        if code in seen:
            raise SystemExit("duplicate Tushare free_share row for {}".format(code))
        seen.add(code)
        rows.append({
            "date": target_date,
            "code": code,
            "free_share": "{:.10f}".format(value).rstrip("0").rstrip("."),
            "unit": "万股",
            "source": "tushare.daily_basic",
            "source_date": source_date,
        })
    rows.sort(key=lambda row: row["code"])
    if not rows:
        raise SystemExit("Tushare returned no positive Shenzhen free_share rows")
    atomic_write(args.output, rows)
    print("source_date={}".format(source_date))
    print("target_date={}".format(target_date))
    print("instruments={}".format(len(rows)))
    print("output={}".format(os.path.abspath(args.output)))


if __name__ == "__main__":
    try:
        main()
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(2)
