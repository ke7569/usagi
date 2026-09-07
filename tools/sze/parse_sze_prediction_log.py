#!/usr/bin/env python3
import argparse
import csv
import json
import os
import struct
import sys
import zlib


FILE_HEADER = struct.Struct("<8sIIIIQ32s")
RECORD_HEADER = struct.Struct("<IHBBIIQII")
COMPACT = struct.Struct("<9sBBhqqqqqqqqqddddfQ")
EVENT = struct.Struct("<9sBBBBhHqqqqdqqqQQQ")
RESOLUTION = struct.Struct("<9sBBBhqqqqdq")
RECORD_MAGIC = 0x474C5A53

FACTOR_NAMES = [
    "factor_spread_permille", "factor_mid_return_permille",
    "factor_weighted_return_permille_1", "factor_weighted_return_permille_2",
    "factor_weighted_return_permille_3", "factor_weighted_return_permille_4",
    "factor_weighted_return_permille_5", "factor_weighted_volume_imbalance",
    "factor_volume_imbalance", "factor_percent_turnover",
    "factor_liquidity_ask_l1_share", "factor_liquidity_bid_l1_share",
    "factor_hermes_permille", "factor_tr_sqrt_positive",
    "factor_fee_on_tick", "factor_bid_volume_change_ratio",
    "factor_ask_volume_change_ratio", "factor_weighted_ask_permille",
    "factor_weighted_bid_permille", "factor_weighted_ask_return_permille",
    "factor_weighted_bid_return_permille", "factor_positive_fill_rate",
    "factor_negative_fill_rate", "factor_order_flow_imbalance",
    "factor_cfr_imbalance", "factor_book_count_imbalance_l1",
    "factor_book_count_imbalance_l5", "factor_book_avg_size_imbalance_l1",
    "factor_book_avg_size_imbalance_l5", "factor_book_life_imbalance_l1",
    "factor_book_life_imbalance_l5", "factor_book_fixdist_imbalance_1pct",
    "factor_book_fixdist_imbalance_5pct", "factor_book_fixdist_weighted_1pct",
    "factor_book_fixdist_weighted_5pct", "factor_book_avg_size_imbalance",
    "factor_book_count_imbalance", "factor_book_life_imbalance",
    "factor_book_young_imbalance_1pct", "factor_max_bid_distance_ratio",
    "factor_max_ask_distance_ratio", "factor_max_vol_distance_imbalance",
    "factor_book_fixdist_hermes", "factor_positive_order_flow_log1p",
    "factor_negative_order_flow_log1p", "factor_market_flow_asinh",
    "factor_cancel_buy_flow_log1p", "factor_cancel_sell_flow_log1p",
    "factor_positive_trade_log1p", "factor_negative_trade_log1p",
]


def symbol(raw):
    return raw.split(b"\0", 1)[0].decode("ascii")


def compact_row(payload):
    values = COMPACT.unpack_from(payload)
    flags = values[1]
    return [
        symbol(values[0]), values[4], values[5], values[6], values[7], values[8],
        values[9], values[10], values[11], values[12], values[13], values[14],
        values[15], values[16], int(bool(flags & 1)), int(bool(flags & 2)),
        int(bool(flags & 4)), values[3], values[17], values[18], values[2],
    ]


COMPACT_HEADER = [
    "instrument", "exchange_time_us", "local_time_us", "app_sequence",
    "cut_index", "row_in_stock_day", "window_start_exchange_time_us",
    "window_start_app_sequence", "window_start_cut_index", "framework_receive_time",
    "last_price", "mid_price", "turnover", "volume", "amount_trigger",
    "time_trigger", "change_trigger", "source", "raw_prediction",
    "model_latency_ns", "source_continuity_valid",
]


def main():
    parser = argparse.ArgumentParser(description="Parse SZE asynchronous prediction logs")
    parser.add_argument("input")
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    os.makedirs(args.output_dir, exist_ok=True)

    paths = {
        "samples": os.path.join(args.output_dir, "samples_compact.csv"),
        "full": os.path.join(args.output_dir, "samples_full.csv"),
        "events": os.path.join(args.output_dir, "events_detail.csv"),
        "resolutions": os.path.join(args.output_dir, "market_resolutions_detail.csv"),
    }
    counts = {"compact_samples": 0, "full_samples": 0, "orders": 0,
              "trades": 0, "market_resolutions": 0}
    last_sequence = 0
    incomplete_tail = False

    with open(args.input, "rb") as source, \
            open(paths["samples"], "w", newline="") as compact_file, \
            open(paths["full"], "w", newline="") as full_file, \
            open(paths["events"], "w", newline="") as event_file, \
            open(paths["resolutions"], "w", newline="") as resolution_file:
        header_bytes = source.read(FILE_HEADER.size)
        if len(header_bytes) != FILE_HEADER.size:
            raise ValueError("truncated prediction log header")
        magic, version, header_size, endian, feature_count, created_ns, _ = \
            FILE_HEADER.unpack(header_bytes)
        if not magic.startswith(b"SZEPLG1") or version != 1 or \
                header_size != FILE_HEADER.size or endian != 0x01020304:
            raise ValueError("unsupported prediction log header")
        if feature_count != len(FACTOR_NAMES):
            raise ValueError("factor count mismatch: {}".format(feature_count))

        compact_writer = csv.writer(compact_file)
        full_writer = csv.writer(full_file)
        event_writer = csv.writer(event_file)
        resolution_writer = csv.writer(resolution_file)
        compact_writer.writerow(COMPACT_HEADER)
        full_writer.writerow(COMPACT_HEADER +
            ["bid_price_{}".format(i) for i in range(1, 11)] +
            ["ask_price_{}".format(i) for i in range(1, 11)] +
            ["bid_volume_{}".format(i) for i in range(1, 11)] +
            ["ask_volume_{}".format(i) for i in range(1, 11)] + FACTOR_NAMES)
        event_writer.writerow([
            "instrument", "event_type", "event_kind", "accepted",
            "source_continuity_valid", "source", "samples_emitted", "app_sequence",
            "exchange_time_us", "local_time_us", "framework_receive_time", "price",
            "volume", "buy_order_id", "sell_order_id", "book_mutation_ns",
            "sample_work_ns", "total_runtime_ns",
        ])
        resolution_writer.writerow([
            "instrument", "from_linked_fill", "buy", "source_continuity_valid",
            "source", "app_sequence", "exchange_time_us", "local_time_us",
            "framework_receive_time", "price", "volume",
        ])

        while True:
            raw_header = source.read(RECORD_HEADER.size)
            if not raw_header:
                break
            if len(raw_header) != RECORD_HEADER.size:
                incomplete_tail = True
                break
            fields = list(RECORD_HEADER.unpack(raw_header))
            magic, rec_version, rec_type, detail, total_bytes, payload_bytes, sequence, \
                payload_crc, header_crc = fields
            fields[-1] = 0
            if magic != RECORD_MAGIC or rec_version != 1 or \
                    total_bytes != RECORD_HEADER.size + payload_bytes or \
                    zlib.crc32(RECORD_HEADER.pack(*fields)) & 0xffffffff != header_crc:
                raise ValueError("invalid record header at sequence {}".format(sequence))
            payload = source.read(payload_bytes)
            if len(payload) != payload_bytes:
                incomplete_tail = True
                break
            if zlib.crc32(payload) & 0xffffffff != payload_crc:
                raise ValueError("payload checksum mismatch at sequence {}".format(sequence))
            if sequence != last_sequence + 1:
                raise ValueError("record sequence gap: expected {}, got {}".format(
                    last_sequence + 1, sequence))
            last_sequence = sequence

            if rec_type in (1, 2):
                row = compact_row(payload)
                compact_writer.writerow(row)
                if rec_type == 1:
                    counts["compact_samples"] += 1
                else:
                    expected = COMPACT.size + 40 * 8 + len(FACTOR_NAMES) * 4
                    if payload_bytes != expected:
                        raise ValueError("full sample size mismatch")
                    book = struct.unpack_from("<40d", payload, COMPACT.size)
                    factors = struct.unpack_from("<{}f".format(len(FACTOR_NAMES)),
                                                 payload, COMPACT.size + 40 * 8)
                    full_writer.writerow(row + list(book) + list(factors))
                    counts["full_samples"] += 1
            elif rec_type in (3, 4):
                values = EVENT.unpack(payload)
                event_writer.writerow([
                    symbol(values[0]), "order" if rec_type == 3 else "trade",
                    values[2], values[3], values[4], values[5], values[6], values[7],
                    values[8], values[9], values[10], values[11], values[12], values[13],
                    values[14], values[15], values[16], values[17],
                ])
                counts["orders" if rec_type == 3 else "trades"] += 1
            elif rec_type == 5:
                values = RESOLUTION.unpack(payload)
                resolution_writer.writerow([symbol(values[0])] + list(values[1:]))
                counts["market_resolutions"] += 1
            else:
                raise ValueError("unknown record type {}".format(rec_type))

    summary = {
        "input": os.path.abspath(args.input), "created_unix_ns": created_ns,
        "last_record_sequence": last_sequence, "incomplete_tail": incomplete_tail,
        "counts": counts, "outputs": paths,
    }
    with open(os.path.join(args.output_dir, "summary.json"), "w") as output:
        json.dump(summary, output, indent=2, sort_keys=True)
        output.write("\n")
    print(json.dumps(summary, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print("error: {}".format(exc), file=sys.stderr)
        sys.exit(1)
