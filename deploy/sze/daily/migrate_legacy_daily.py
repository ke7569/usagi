#!/usr/bin/env python3
"""Convert an old all-in-one SZE config to the strict daily JSON schema."""

import argparse
import copy
import hashlib
import json
import os
import tempfile


DAILY_KEYS = {"trading_day", "static_data_source_date", "static_data_hash",
              "ins_params"}


def canonical_hash(value):
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=True).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def atomic_write(path, value):
    parent = os.path.dirname(os.path.abspath(path)) or "."
    if not os.path.isdir(parent):
        os.makedirs(parent)
    fd, temporary = tempfile.mkstemp(prefix=".sze-daily-", dir=parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(value, stream, ensure_ascii=True, indent=2,
                      sort_keys=True)
            stream.write("\n")
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def convert(source, target_day, source_day):
    if not isinstance(source.get("ins_params"), dict) or not source["ins_params"]:
        raise ValueError("legacy config has no non-empty ins_params")
    params = copy.deepcopy(source["ins_params"])
    for symbol, item in params.items():
        if not isinstance(item, dict):
            raise ValueError("ins_params.{} must be an object".format(symbol))
        item.pop("cpu", None)
        item.pop("last_position", None)
        if "Date" in item:
            item["Date"] = int(target_day)
    return {
        "trading_day": int(target_day),
        "static_data_source_date": int(
            source_day if source_day is not None else
            source.get("static_data_source_date", target_day)),
        "static_data_hash": canonical_hash(params),
        "ins_params": params,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("target")
    parser.add_argument("--trading-day", required=True, type=int)
    parser.add_argument("--source-date", type=int)
    args = parser.parse_args()
    with open(args.source, encoding="utf-8") as stream:
        source = json.load(stream)
    daily = convert(source, args.trading_day, args.source_date)
    atomic_write(args.target, daily)
    print(json.dumps({"ok": True, "trading_day": daily["trading_day"],
                      "instruments": len(daily["ins_params"]),
                      "static_data_hash": daily["static_data_hash"],
                      "output": os.path.abspath(args.target)},
                     sort_keys=True, separators=(",", ":")))


if __name__ == "__main__":
    main()
