#!/usr/bin/env python3
"""Export bundled raw SZE Arrow and all V06 factor goldens for C++ replay.

No golden values are used to construct or filter the raw event stream. This
fixture is offline-only and deliberately includes the pre-open order book.
"""
import argparse
import json
import struct
from pathlib import Path

import pyarrow as pa


def read_arrow(path):
    with pa.memory_map(str(path), "r") as source:
        try:
            return pa.ipc.open_file(source).read_all()
        except pa.ArrowInvalid:
            source.seek(0)
            return pa.ipc.open_stream(source).read_all()


def integer(value):
    if isinstance(value, (bytes, str)):
        return ord(value) if len(value) == 1 else int(value)
    return int(value)


def table_rows(table):
    # Arrow 6 is the last release available for the production host's Python 3.6.
    columns = table.to_pydict()
    names = list(columns)
    return [dict(zip(names, values)) for values in zip(*(columns[name] for name in names))]


def export(bundle, output):
    case = bundle / "golden/000807.SZ_20260401"
    static = json.loads((case / "case_parameters.json").read_text())["stock_day_parameters"]
    names = json.loads((bundle / "factors/factor_contract.json").read_text())["feature_names"]
    assert len(names) == 50
    if names != (bundle / "factors/factors.txt").read_text().splitlines():
        raise ValueError("factor contract order differs from factors.txt")
    raw = bundle / "raw/20260401/stocks/000807.SZ"
    orders = table_rows(read_arrow(raw / "order.arrow"))
    trades = table_rows(read_arrow(raw / "trade.arrow"))
    events = [(1, row) for row in orders] + [(2, row) for row in trades]
    events.sort(key=lambda item: int(item[1]["app_seq"]))
    if len({int(row["app_seq"]) for _, row in events}) != len(events):
        raise ValueError("duplicate raw AppSeq")
    golden = table_rows(read_arrow(case / "features.arrow"))
    if len(golden) != int(static["row_len"]) or len(golden) != 12852:
        raise ValueError("unexpected V06 golden sample count")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as out:
        out.write(struct.pack("<8sIII", b"V06FACT1", 50, len(events), len(golden)))
        out.write(struct.pack("<i6d", int(static["date"]), *[
            float(static[k]) for k in ("avg_amount", "turnover_threshold", "free_share",
                                       "pre_close", "limit_price", "stop_price")]))
        for kind, row in events:
            out.write(struct.pack("<Bqqqdq", kind, int(row["app_seq"]), int(row["ex_time"]),
                                  int(row["timestamp"]), float(row["price"]), int(row["volume"])))
            typ = integer(row["type_char"])
            if kind == 1:
                if typ not in (ord("1"), ord("2"), ord("U")):
                    raise ValueError("unknown order type: %r" % typ)
                out.write(struct.pack("<BB", int(integer(row["direction"]) == 1),
                                      1 if typ == ord("1") else 2 if typ == ord("U") else 0))
            else:
                if typ not in (ord("F"), ord("4")):
                    raise ValueError("unknown trade type: %r" % typ)
                out.write(struct.pack("<qqB", int(row["buy_no"]), int(row["sell_no"]), int(typ == ord("F"))))
        for row in golden:
            out.write(struct.pack("<6q", *[int(row[k]) for k in (
                "ex_time_micros", "app_seq", "cut_index", "window_start_ex_time_micros",
                "window_start_app_seq", "window_start_cut_index")]))
            out.write(struct.pack("<3B", *[int(row["analysis_sampling_trigger_" + k]) for k in ("amount", "time", "change")]))
            out.write(struct.pack("<50f", *[float(row[k]) for k in names]))
    print(json.dumps({"events": len(events), "expected_samples": len(golden), "output": str(output)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    export(args.bundle_root, args.output)
