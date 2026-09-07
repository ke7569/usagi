#!/usr/bin/env python3
"""Export an offline golden fixture for the native mix153060 C++ runtime.

This utility is test-only. The generated fixture and Python/Arrow dependencies
are not part of the live strategy process.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path

import pyarrow.compute as pc

from mix153060_reproduce import (
    FACTOR_NAMES,
    instrument_text,
    load_static_rows,
    read_arrow,
    reference_history_volatility,
    timeline_events,
)


HEADER = struct.Struct("<8sIIII")
GROUP_HEADER = struct.Struct("<I16si6dII")
COMMON_EVENT = struct.Struct("<qqqdq")
ORDER_FIELDS = struct.Struct("<BB")
TRADE_FIELDS = struct.Struct("<qqB")
EXPECTED_IDENTITY = struct.Struct("<7q")


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_common(handle, row):
    handle.write(
        COMMON_EVENT.pack(
            int(row["app_seq"]),
            int(row["ex_time"]),
            int(row["timestamp"]),
            float(row["price"]),
            int(row["volume"]),
        )
    )


def write_trade(handle, row):
    write_common(handle, row)
    handle.write(
        TRADE_FIELDS.pack(
            int(row["buy_no"]),
            int(row["sell_no"]),
            1 if int(row["type_char"]) == ord("F") else 0,
        )
    )


def write_frame(handle, frame):
    kind, row, fills = frame
    handle.write(struct.pack("<BI", 1 if kind == "order" else 2, len(fills)))
    write_common(handle, row)
    if kind == "order":
        type_char = int(row["type_char"])
        order_kind = 1 if type_char == ord("1") else 2 if type_char == ord("U") else 0
        handle.write(ORDER_FIELDS.pack(1 if int(row["direction"]) == 1 else 0, order_kind))
        for fill in fills:
            write_trade(handle, fill)
    else:
        handle.write(
            TRADE_FIELDS.pack(
                int(row["buy_no"]),
                int(row["sell_no"]),
                1 if int(row["type_char"]) == ord("F") else 0,
            )
        )


def selected_expected(
    golden,
    stock_order,
    rows_per_stock,
    start_ex_time_us=None,
    end_ex_time_us=None,
):
    selected = golden.filter(pc.equal(golden["selection_order"], stock_order))
    if start_ex_time_us is not None:
        selected = selected.filter(
            pc.greater_equal(selected["ex_time_micros"], start_ex_time_us)
        )
    if end_ex_time_us is not None:
        selected = selected.filter(pc.less(selected["ex_time_micros"], end_ex_time_us))
    if rows_per_stock > 0:
        selected = selected.slice(0, rows_per_stock)
    return selected


def export_fixture(
    bundle,
    output,
    rows_per_stock,
    start_ex_time_us=None,
    end_ex_time_us=None,
):
    if (start_ex_time_us is not None and end_ex_time_us is not None and
            start_ex_time_us >= end_ex_time_us):
        raise ValueError("start exchange time must be before end exchange time")
    selection = json.loads((bundle / "raw/20260715/selection.json").read_text())
    golden = read_arrow(bundle / "golden/features.arrow")
    static_rows = load_static_rows(bundle)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    total_frames = 0
    total_rows = 0
    with temporary.open("wb") as handle:
        handle.write(HEADER.pack(b"MIXRUN01", 1, len(selection["stocks"]), len(FACTOR_NAMES), rows_per_stock))
        for stock in selection["stocks"]:
            stock_order = int(stock["selection_order"])
            instrument = str(stock["instrument_id"])
            raw_root = bundle / "raw/20260715/stocks" / instrument
            orders = read_arrow(raw_root / "order.arrow").to_pylist()
            trades = read_arrow(raw_root / "trade.arrow").to_pylist()
            frames = timeline_events(orders, trades)
            expected = selected_expected(
                golden,
                stock_order,
                rows_per_stock,
                start_ex_time_us=start_ex_time_us,
                end_ex_time_us=end_ex_time_us,
            )
            if expected.num_rows == 0:
                raise ValueError("no golden rows for stock_order={}".format(stock_order))
            if rows_per_stock > 0 or start_ex_time_us is not None or end_ex_time_us is not None:
                last_cut = int(expected["cut_index"][-1].as_py())
                frames = frames[: last_cut + 1]
            static = static_rows[instrument_text(instrument)]
            volatility = reference_history_volatility(golden, stock_order)
            handle.write(
                GROUP_HEADER.pack(
                    stock_order,
                    instrument.encode("ascii").ljust(16, b"\0"),
                    20260715,
                    float(static["avg_amount"]),
                    float(static["turnover_threshold"]),
                    float(static["pre_close"]),
                    float(static["limit_price"]),
                    float(static["stop_price"]),
                    volatility,
                    len(frames),
                    expected.num_rows,
                )
            )
            for frame in frames:
                write_frame(handle, frame)
            expected_rows = expected.select(
                [
                    "row_in_stock_day",
                    "ex_time_micros",
                    "app_seq",
                    "cut_index",
                    "window_start_ex_time_micros",
                    "window_start_app_seq",
                    "window_start_cut_index",
                ]
                + list(FACTOR_NAMES)
            ).to_pylist()
            for row in expected_rows:
                handle.write(
                    EXPECTED_IDENTITY.pack(
                        int(row["row_in_stock_day"]),
                        int(row["ex_time_micros"]),
                        int(row["app_seq"]),
                        int(row["cut_index"]),
                        int(row["window_start_ex_time_micros"]),
                        int(row["window_start_app_seq"]),
                        int(row["window_start_cut_index"]),
                    )
                )
                handle.write(struct.pack("<50f", *[float(row[name]) for name in FACTOR_NAMES]))
            total_frames += len(frames)
            total_rows += expected.num_rows
    temporary.replace(output)
    return {
        "output": str(output.resolve()),
        "sha256": file_sha256(output),
        "groups": len(selection["stocks"]),
        "frames": total_frames,
        "expected_rows": total_rows,
        "rows_per_stock": rows_per_stock,
        "start_ex_time_us": start_ex_time_us,
        "end_ex_time_us": end_ex_time_us,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--rows-per-stock",
        type=int,
        default=3,
        help="0 exports the full sequence; positive values keep the first N golden rows",
    )
    parser.add_argument(
        "--start-ex-time-us",
        type=int,
        help="inclusive comparison bound; frames before it remain as warm-up",
    )
    parser.add_argument(
        "--end-ex-time-us",
        type=int,
        help="exclusive comparison bound; replay stops after the last needed frame",
    )
    args = parser.parse_args()
    report = export_fixture(
        args.bundle_root.resolve(),
        args.output.resolve(),
        max(args.rows_per_stock, 0),
        start_ex_time_us=args.start_ex_time_us,
        end_ex_time_us=args.end_ex_time_us,
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
