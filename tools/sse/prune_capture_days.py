#!/usr/bin/env python
"""Remove the oldest completed day under an explicitly supplied capture root.

No deletion occurs unless space is below the threshold and --delete is given.
Current/future days, symlinked day directories and non-date names are excluded.
"""
from __future__ import print_function
import argparse
import datetime
import json
import os
import re
import shutil
import sys


def available(path):
    stat = os.statvfs(path)
    return stat.f_bavail * stat.f_frsize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--keep-day", required=True)
    parser.add_argument("--min-free-gib", type=int, default=100)
    parser.add_argument("--delete", action="store_true")
    args = parser.parse_args()
    root = os.path.realpath(args.root)
    keep = datetime.datetime.strptime(args.keep_day, "%Y%m%d").date()
    if root in ("/", "/home", "/home/zane", "/root") or args.min_free_gib < 1:
        raise ValueError("a dedicated capture root and positive reserve are required")
    if not os.path.isdir(root):
        print(json.dumps({"event": "retention", "root": root, "removed": [], "state": "root-not-created"}))
        return 0
    threshold = args.min_free_gib * (1 << 30)
    candidates = []
    for name in os.listdir(root):
        if not re.match(r"^[0-9]{8}$", name):
            continue
        try:
            day = datetime.datetime.strptime(name, "%Y%m%d").date()
        except ValueError:
            continue
        path = os.path.join(root, name)
        if day < keep and day < datetime.date.today() and os.path.isdir(path) and not os.path.islink(path) and os.path.realpath(path) == path:
            candidates.append((day, path))
    removed = []
    for day, path in sorted(candidates):
        if available(root) >= threshold:
            break
        if not args.delete:
            print(json.dumps({"event": "retention_candidate", "path": path, "delete": False}))
            continue
        # Recheck the exact target immediately before recursive removal.
        if os.path.dirname(path) != root or os.path.realpath(path) != path or os.path.islink(path):
            raise RuntimeError("capture day target changed during retention")
        shutil.rmtree(path)
        removed.append(path)
        print(json.dumps({"event": "retention_deleted", "path": path, "recoverable": False}))
    free = available(root)
    print(json.dumps({"event": "retention", "root": root, "removed": removed,
                      "available_bytes": free, "minimum_bytes": threshold}))
    return 0 if free >= threshold else 1


if __name__ == "__main__":
    sys.exit(main())
