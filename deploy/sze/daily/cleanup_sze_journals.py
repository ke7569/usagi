#!/usr/bin/env python3
"""Remove Shenzhen journal directories older than the configured retention."""

import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys


JOURNAL_RE = re.compile(r"^sze_journal_(\d{8})$")
ACTIVE_UNIT_RE = re.compile(
    r"^(?:sze-(?:capture|recovery|trade)(?:@.*)?|"
    r"sze-all-(?:capture|recovery).*|sze-recovery-trade-).*\.service$")


def active_sze_units():
    try:
        result = subprocess.run(
            ["systemctl", "list-units", "--state=active", "--no-legend",
             "--no-pager"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
            check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError("cannot inspect systemd active units: {}".format(error))
    units = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if fields and ACTIVE_UNIT_RE.match(fields[0]):
            units.append(fields[0])
    return sorted(set(units))


def candidate_directories(root, cutoff):
    root = os.path.realpath(root)
    if not os.path.isdir(root):
        raise RuntimeError("journal root is not a directory: {}".format(root))
    candidates = []
    for name in os.listdir(root):
        match = JOURNAL_RE.match(name)
        if not match:
            continue
        path = os.path.join(root, name)
        if os.path.islink(path) or not os.path.isdir(path):
            continue
        try:
            journal_date = datetime.datetime.strptime(
                match.group(1), "%Y%m%d").date()
        except ValueError:
            continue
        if journal_date < cutoff:
            real_path = os.path.realpath(path)
            if os.path.dirname(real_path) != root:
                raise RuntimeError("refusing journal path outside root: {}".format(path))
            candidates.append((journal_date, path))
    return sorted(candidates)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="/home/zane/data")
    parser.add_argument("--retention-days", type=int, default=3)
    parser.add_argument("--today")
    parser.add_argument("--delete", action="store_true",
                        help="delete candidates; without this flag only dry-runs")
    args = parser.parse_args()
    if args.retention_days < 1:
        raise SystemExit("retention-days must be positive")
    today = (datetime.datetime.strptime(args.today, "%Y%m%d").date()
             if args.today else datetime.date.today())
    cutoff = today - datetime.timedelta(days=args.retention_days)
    units = active_sze_units()
    if units:
        print("journal_cleanup_refused active_units={}".format(",".join(units)),
              file=sys.stderr)
        return 2
    candidates = candidate_directories(args.root, cutoff)
    action = "delete" if args.delete else "dry-run"
    print("journal_cleanup action={} today={} cutoff={} retention_days={} candidates={}".format(
        action, today.strftime("%Y%m%d"), cutoff.strftime("%Y%m%d"),
        args.retention_days, len(candidates)))
    for journal_date, path in candidates:
        print("{} {}".format(journal_date.strftime("%Y%m%d"), path))
        if args.delete:
            shutil.rmtree(path)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print("journal_cleanup_error={}".format(error), file=sys.stderr)
        sys.exit(1)
