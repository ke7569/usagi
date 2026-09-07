#!/usr/bin/env python3
"""Convert a bounded mix153060 Arrow replay into T0 backtest CSV files.

The converter is offline tooling.  It preserves first-event warm-up and emits
the legacy ``helper.cpp`` column layout so the existing BacktestEngine can be
used for a smoke replay without putting Arrow/Python in the live plugin.
"""

import argparse
import csv
import datetime
import json
from pathlib import Path

from mix153060_reproduce import read_arrow, timeline_events


ORDER_HEADER = (
    "OrderTime", "ExchangeID", "InstrumentID", "Price", "Volume", "OrderKind",
    "ApplSeqNum", "OrdType", "OrderNo", "BizIndex", "rank", "isLast",
)
TRADE_HEADER = (
    "TradeTime", "ExchangeID", "InstrumentID", "Price", "Volume", "OrderKind",
    "OrderBSFlag", "Turnover", "BidApplSeqNum", "OfferApplSeqNum", "ApplSeqNum",
    "BizIndex", "rank", "isLast",
)


def exchange_time(epoch_us):
    value = datetime.datetime.utcfromtimestamp(float(epoch_us) / 1000000.0)
    return value.strftime("%Y%m%d%H%M%S") + "{:03d}".format(value.microsecond // 1000)


def selected_frames(golden, stock_order, events, end_ex_time_us):
    if end_ex_time_us is None:
        return events
    import pyarrow.compute as pc

    expected = golden.filter(pc.equal(golden["selection_order"], stock_order))
    expected = expected.filter(pc.less(expected["ex_time_micros"], end_ex_time_us))
    if expected.num_rows == 0:
        return []
    last_cut = int(expected["cut_index"][-1].as_py())
    return events[: last_cut + 1]


def convert(bundle, order_output, trade_output, start_ex_time_us, end_ex_time_us):
    if (start_ex_time_us is not None and end_ex_time_us is not None and
            start_ex_time_us >= end_ex_time_us):
        raise ValueError("start exchange time must be before end exchange time")
    selection = json.loads((bundle / "raw/20260715/selection.json").read_text())
    golden = read_arrow(bundle / "golden/features.arrow")
    frames = []
    warmup_frames = 0
    for stock in selection["stocks"]:
        instrument = str(stock["instrument_id"])
        stock_order = int(stock["selection_order"])
        root = bundle / "raw/20260715/stocks" / instrument
        orders = read_arrow(root / "order.arrow").to_pylist()
        trades = read_arrow(root / "trade.arrow").to_pylist()
        timeline = timeline_events(orders, trades)
        for ordinal, frame in enumerate(
            selected_frames(golden, stock_order, timeline, end_ex_time_us)
        ):
            kind, row, fills = frame
            watermark = fills[-1] if kind == "order" and fills else row
            watermark_time = int(watermark["ex_time"])
            if start_ex_time_us is not None and watermark_time < start_ex_time_us:
                warmup_frames += 1
            frames.append((
                int(row["app_seq"]), watermark_time, stock_order, ordinal,
                kind, row, fills,
            ))

    frames.sort(key=lambda value: (value[0], value[2], value[3]))
    order_rows = []
    trade_rows = []
    rank = 0
    for _, _, stock_order, _, kind, row, fills in frames:
        instrument = str(row.get("instrument_id", ""))
        # Arrow metadata carries the instrument; the row itself does not.
        if not instrument:
            instrument = str(selection["stocks"][stock_order]["instrument_id"])
        numeric_instrument = instrument.split(".", 1)[0]
        if kind == "order":
            raw_order_type = chr(int(row["type_char"]))
            order_type = 1 if raw_order_type == "1" else 4 if raw_order_type == "U" else 2
            order_rows.append((
                exchange_time(int(row["ex_time"])), 1, numeric_instrument,
                float(row["price"]), int(row["volume"]),
                0 if int(row["direction"]) == 1 else 1,
                int(row["app_seq"]), order_type, int(row["app_seq"]), rank, rank, 1,
            ))
            rank += 1
            for fill in fills:
                buy_no = int(fill["buy_no"])
                sell_no = int(fill["sell_no"])
                side = 0 if buy_no > sell_no else 1
                trade_rows.append((
                    exchange_time(int(fill["ex_time"])), 1, numeric_instrument,
                    float(fill["price"]), int(fill["volume"]),
                    0 if int(fill["type_char"]) == ord("F") else 1,
                    side, float(fill["price"]) * int(fill["volume"]),
                    buy_no, sell_no, int(fill["app_seq"]), rank, rank, 1,
                ))
                rank += 1
        else:
            buy_no = int(row["buy_no"])
            sell_no = int(row["sell_no"])
            side = 0 if buy_no > sell_no else 1
            trade_rows.append((
                exchange_time(int(row["ex_time"])), 1, numeric_instrument,
                float(row["price"]), int(row["volume"]),
                0 if int(row["type_char"]) == ord("F") else 1,
                side, float(row["price"]) * int(row["volume"]),
                buy_no, sell_no, int(row["app_seq"]), rank, rank, 1,
            ))
            rank += 1

    order_output.parent.mkdir(parents=True, exist_ok=True)
    with order_output.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(ORDER_HEADER)
        writer.writerows(order_rows)
    with trade_output.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(TRADE_HEADER)
        writer.writerows(trade_rows)
    return {
        "order_output": str(order_output.resolve()),
        "trade_output": str(trade_output.resolve()),
        "orders": len(order_rows),
        "trades": len(trade_rows),
        "frames": len(frames),
        "warmup_frames": warmup_frames,
        "start_ex_time_us": start_ex_time_us,
        "end_ex_time_us": end_ex_time_us,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-root", required=True, type=Path)
    parser.add_argument("--order-output", required=True, type=Path)
    parser.add_argument("--trade-output", required=True, type=Path)
    parser.add_argument("--start-ex-time-us", type=int)
    parser.add_argument("--end-ex-time-us", type=int)
    args = parser.parse_args()
    report = convert(
        args.bundle_root.resolve(),
        args.order_output.resolve(),
        args.trade_output.resolve(),
        args.start_ex_time_us,
        args.end_ex_time_us,
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
