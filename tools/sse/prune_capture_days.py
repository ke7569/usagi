#!/usr/bin/env python
"""Retain five capture dates; preserve TD/models/profiles. Requires --apply."""
from __future__ import print_function

import argparse
import datetime
import errno
import json
import os
import re
import shutil
import sys


def within(path, directory):
    return path == directory or path.startswith(directory + os.sep)


def last_json(path):
    try:
        with open(path, 'rb') as source:
            source.seek(0, os.SEEK_END)
            source.seek(max(0, source.tell() - 65536))
            lines = source.read().decode('utf-8', 'replace').splitlines()
    except (IOError, OSError):
        return None
    for line in reversed(lines):
        try:
            value = json.loads(line)
            if isinstance(value, dict):
                return value
        except (ValueError, TypeError):
            pass
    return None


def capture_epochs(day_path):
    epochs = []
    if os.path.isdir(os.path.join(day_path, 'journal')):
        epochs.append(day_path)  # Original one-epoch-per-day layout.
    for name in sorted(os.listdir(day_path)):
        path = os.path.join(day_path, name)
        if (name.startswith('capture-') and os.path.isdir(path) and
                not os.path.islink(path) and os.path.isdir(os.path.join(path, 'journal'))):
            epochs.append(path)
    return epochs


def capture_targets(day_path):
    """Return only files/directories owned by capture under one day.

    A trading-day directory also contains TD query output, OMS journals,
    profiles and model metadata. Retention must never remove that material.
    """
    targets = []
    epochs = capture_epochs(day_path)
    for epoch in epochs:
        journal = os.path.join(epoch, 'journal')
        if not os.path.islink(journal):
            targets.append(journal)
        for name in ('capture.json', 'capture.pid', 'capture.stdout', 'capture.stderr',
                     'stream_capture.json'):
            path = os.path.join(epoch, name)
            if os.path.isfile(path) and not os.path.islink(path):
                targets.append(path)
    # current is a launcher pointer, never a data store. Remove it only when
    # the containing date is being pruned so no dangling pointer remains.
    current = os.path.join(day_path, 'current')
    if os.path.islink(current) and os.path.realpath(current) in epochs:
        targets.append(current)
    return targets


def tree_reason(path):
    for directory, dirs, files in os.walk(path, followlinks=False):
        for name in dirs + files:
            target = os.path.join(directory, name)
            if os.path.islink(target):
                # The launcher intentionally creates this pointer inside a day.
                if (directory == path and name == 'current' and
                        within(os.path.realpath(target), path)):
                    continue
                return 'symlink'
    return None


def open_days(root, proc_root='/proc'):
    """Refuse deletion when /proc cannot be fully inspected."""
    protected = set()
    for pid in os.listdir(proc_root):
        if not pid.isdigit():
            continue
        fd_root = os.path.join(proc_root, pid, 'fd')
        try:
            descriptors = os.listdir(fd_root)
        except OSError as error:
            if error.errno == errno.ENOENT:
                continue
            raise
        for descriptor in descriptors:
            try:
                target = os.readlink(os.path.join(fd_root, descriptor))
            except OSError as error:
                if error.errno == errno.ENOENT:
                    continue
                raise
            if target.endswith(' (deleted)'):
                target = target[:-10]
            if within(target, root) and target != root:
                protected.add(target[len(root) + 1:].split(os.sep, 1)[0])
    return protected


def plan(root, keep_days, today, opened=None):
    if keep_days < 1:
        raise ValueError('--keep-days must be positive')
    actual = os.path.realpath(root)
    if actual != os.path.abspath(root) or actual in ('/', '/home', '/home/zane', '/root'):
        raise ValueError('a real, dedicated capture root is required')
    if not os.path.isdir(root):
        raise ValueError('capture root does not exist')
    active = os.path.realpath(os.path.join(root, 'active_capture'))
    opened = open_days(root) if opened is None else opened
    dates = []
    ignored = []
    for name in sorted(os.listdir(root)):
        if not re.match(r'^[0-9]{8}$', name):
            continue
        try:
            parsed = datetime.datetime.strptime(name, '%Y%m%d').date()
        except ValueError:
            continue
        path = os.path.join(root, name)
        if os.path.islink(path):
            ignored.append({'day': name, 'reason': 'symlink'})
        elif os.path.isdir(path) and capture_epochs(path):
            dates.append((name, parsed, path))
    # Existing captured dates are the trading-day calendar, so weekends and
    # holidays without capture data do not consume retention slots.
    newest = set(row[0] for row in sorted(
        [row for row in dates if row[1] <= today], reverse=True)[:keep_days])
    result = list(ignored)
    for name, parsed, path in dates:
        if parsed >= today:
            reason = 'current-or-future'
        elif name in newest:
            reason = 'newest-%s' % keep_days
        elif within(active, path):
            reason = 'active-capture'
        elif name in opened:
            reason = 'open-by-process'
        else:
            # A stopped historical day may lack a final status after a crash;
            # absence of that status must not make retention grow forever.
            # Live writers are protected separately by open_days().
            reason = tree_reason(path)
        targets = capture_targets(path)
        result.append({'day': name, 'path': path, 'targets': targets,
                       'empty_directories': [p for p in capture_epochs(path) if p != path] + [path],
                       'action': 'keep' if reason else 'delete',
                       'reason': reason or 'outside-retention'})
    return sorted(result, key=lambda item: item['day'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--keep-days', type=int, default=5)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args(argv)
    os.environ['TZ'] = 'Asia/Shanghai'
    if hasattr(__import__('time'), 'tzset'):
        __import__('time').tzset()
    root = os.path.abspath(args.root)
    rows = plan(root, args.keep_days, datetime.date.today())
    removed = []
    for row in rows:
        if row.get('action') == 'delete' and args.apply:
            # Recheck exact targets and all live protections immediately before
            # deleting. Newer-day arrival only ever makes this more conservative.
            current = dict((r['day'], r) for r in
                           plan(root, args.keep_days, datetime.date.today()))
            fresh = current.get(row['day'], {})
            path = row['path']
            targets = fresh.get('targets', [])
            if (fresh.get('action') != 'delete' or
                    os.path.dirname(path) != root or os.path.realpath(path) != path or
                    any(target == path or not within(target, path) for target in targets)):
                row['action'] = 'keep'
                row['reason'] = 'changed-during-check'
            else:
                for target in targets:
                    if not os.path.islink(target) and os.path.realpath(target) != target:
                        raise ValueError('capture target changed during deletion: ' + target)
                    if os.path.isdir(target) and not os.path.islink(target):
                        shutil.rmtree(target)
                    elif os.path.lexists(target):
                        os.unlink(target)
                # Only empty containers disappear; unknown files in a capture
                # epoch or in the date directory are deliberately preserved.
                for directory in fresh.get('empty_directories', []):
                    if (not within(directory, path) or os.path.realpath(directory) != directory):
                        raise ValueError('capture container changed during deletion: ' + directory)
                    try:
                        os.rmdir(directory)
                    except OSError as error:
                        if error.errno not in (errno.ENOTEMPTY, errno.EEXIST, errno.ENOENT):
                            raise
                row['action'] = 'removed'
                removed.append(row['day'])
        print(json.dumps(row, sort_keys=True))
    print(json.dumps({'event': 'capture_retention', 'apply': args.apply,
                      'keep_days': args.keep_days, 'removed': removed,
                      'recoverable': False if removed else None}, sort_keys=True))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (IOError, OSError, ValueError) as error:
        print('capture retention failed: ' + str(error), file=sys.stderr)
        sys.exit(1)
