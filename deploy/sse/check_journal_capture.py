#!/usr/bin/env python
"""Check capture readiness, two-channel growth, or the complete day's close."""
from __future__ import print_function

import argparse
import datetime
import json
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sse'))
from prune_capture_days import capture_epochs, last_json, within


def counts(status):
    rows = status.get('channels', [])
    result = dict((row['name'], int(row['datagrams'])) for row in rows)
    if set(result) != set(('sse_tick', 'sse_snapshot')) or len(rows) != 2:
        raise ValueError('expected exactly sse_tick and sse_snapshot')
    return result


def live_status(epoch, max_age=20):
    path = os.path.join(epoch, 'capture.stderr')
    value = last_json(path)
    if not value or time.time() - os.stat(path).st_mtime > max_age:
        raise ValueError('capture status is missing or stale')
    for field in ('ready', 'input_valid', 'processing_valid'):
        if value.get(field) is not True:
            raise ValueError(field + ' is not true')
    for field in ('journal_errors', 'journal_overflows'):
        if value.get(field) != 0:
            raise ValueError(field + ' is not zero')
    for field in ('journal_degraded', 'recording_failed', 'stop_requested'):
        if value.get(field) is not False:
            raise ValueError(field + ' is not false')
    counts(value)
    return value


def check_closed(day_path):
    epochs = capture_epochs(day_path)
    if not epochs:
        raise ValueError('no capture epochs for this day')
    total = {'sse_tick': 0, 'sse_snapshot': 0}
    rows = []
    good = True
    for epoch in epochs:
        final = last_json(os.path.join(epoch, 'capture.stdout')) or {}
        stats = final.get('stats', {})
        row = {'epoch': os.path.basename(epoch), 'ok': final.get('ok'),
               'journal_clean': stats.get('journal_clean'),
               'journal_errors': final.get('journal_errors'),
               'journal_overflows': final.get('journal_overflows'),
               'kernel_drops': stats.get('kernel_drops')}
        row['valid'] = (row['ok'] is True and row['journal_clean'] is True and
                        row['journal_errors'] == 0 and row['journal_overflows'] == 0 and
                        row['kernel_drops'] == 0 and
                        final.get('accepted_events') is not None and
                        final.get('accepted_events') == final.get('journal_events'))
        try:
            channel_counts = counts(final)
            row['channels'] = channel_counts
            for name, count in channel_counts.items():
                total[name] += count
        except (ValueError, KeyError, TypeError):
            row['valid'] = False
        rows.append(row)
        good = good and row['valid']
    good = good and all(count > 0 for count in total.values())
    return {'ok': bool(good), 'channels': total, 'epochs': rows}


def main(argv=None):
    os.environ['TZ'] = 'Asia/Shanghai'
    time.tzset()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('preopen', 'intraday', 'closed'))
    parser.add_argument('--root', default='/home/zane/usagi-runtime')
    parser.add_argument('--day', default=datetime.date.today().strftime('%Y%m%d'))
    parser.add_argument('--service', default='sse-journal-capture.service')
    parser.add_argument('--interval', type=float, default=6)
    args = parser.parse_args(argv)
    datetime.datetime.strptime(args.day, '%Y%m%d')
    if not 5 <= args.interval <= 30:
        raise ValueError('--interval must be between 5 and 30 seconds')
    day_path = os.path.join(os.path.abspath(args.root), args.day)
    if args.mode == 'closed':
        result = check_closed(day_path)
    else:
        with open(os.devnull, 'w') as devnull:
            if subprocess.call(['systemctl', 'is-active', '--quiet', args.service],
                               stdout=devnull, stderr=devnull) != 0:
                raise ValueError('capture service is not active')
        current = os.path.join(day_path, 'current')
        epoch = os.path.realpath(current)
        if not os.path.islink(current) or not within(epoch, day_path):
            raise ValueError('current capture epoch is missing or outside day directory')
        first = live_status(epoch)
        before = counts(first)
        result = {'ok': True, 'epoch': epoch, 'ready': True,
                  'journal_errors': 0, 'journal_overflows': 0, 'channels': before}
        if args.mode == 'intraday':
            time.sleep(args.interval)
            if epoch != os.path.realpath(current):
                raise ValueError('capture restarted during check')
            after = counts(live_status(epoch))
            growth = dict((name, after[name] - before[name]) for name in before)
            result.update(channels=after, growth=growth,
                          ok=all(value > 0 for value in growth.values()))
    result.update(event='capture_check', day=args.day, mode=args.mode)
    print(json.dumps(result, sort_keys=True))
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (IOError, OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps({'event': 'capture_check', 'ok': False, 'error': str(error)},
                         sort_keys=True))
        sys.exit(1)

