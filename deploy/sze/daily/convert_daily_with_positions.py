#!/usr/bin/env python3
"""Convert a legacy SZE all-in-one config to the compact daily format."""

import datetime
import hashlib
import json
import os
import sys
import tempfile


STATIC_KEYS = (
    "Close", "HistoryAmount", "FreeShare", "HpUpperPrice",
    "HpLowerPrice", "HistoryVolatility20d")


def load(path):
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)


def canonical_hash(value):
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def write_atomic(path, value):
    parent = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(parent):
        os.makedirs(parent)
    fd, temporary = tempfile.mkstemp(prefix=".sze-daily-", dir=parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: convert_daily_with_positions.py SOURCE PRIOR OUTPUT")
    source_path, prior_path, output_path = sys.argv[1:]
    source = load(source_path)
    prior = load(prior_path)
    day = int(source["trading_day"])
    if day != int(os.path.basename(output_path).split("_")[-1].split(".")[0]):
        raise ValueError("output filename trading day does not match source")
    source_params = source.get("ins_params") or {}
    prior_params = prior.get("ins_params") or {}
    positions = {
        symbol: int(item.get("static_position", 0))
        for symbol, item in prior_params.items()
        if int(item.get("static_position", 0)) != 0
    }
    missing = sorted(symbol for symbol in positions if symbol not in source_params)
    if missing:
        raise ValueError("prior positions missing from new universe: {}".format(
            ",".join(missing)))
    if not positions:
        raise ValueError("prior config contains no nonzero static_position")
    params = {}
    for symbol, old_item in sorted(source_params.items()):
        item = {key: old_item[key] for key in STATIC_KEYS}
        item["Date"] = day
        item["static_position"] = positions.get(symbol, 0)
        params[symbol] = item
    daily = {
        "trading_day": day,
        "static_data_source_date": int(source.get("static_data_source_date", day)),
        "static_data_hash": canonical_hash(params),
        "ins_params": params,
    }
    write_atomic(output_path, daily)
    print(json.dumps({
        "trading_day": day,
        "instruments": len(params),
        "trade_instruments": len(positions),
        "static_data_hash": daily["static_data_hash"],
        "inherited_from": prior_path,
    }, sort_keys=True, separators=(",", ":")))


if __name__ == "__main__":
    try:
        main()
    except (KeyError, OSError, ValueError) as error:
        raise SystemExit("sze_daily_conversion_error: {}".format(error))
