#!/usr/bin/env python
from __future__ import print_function

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time


TRADING_DAY = int(time.strftime('%Y%m%d'))
SHARD_COUNT = 3
CHANNELS = tuple(range(1, 7))
STOCKS_PER_CHANNEL = 7
EVENT_COUNT = len(CHANNELS) * STOCKS_PER_CHANNEL * 2
DURATION_MS = 1800


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def packet_bytes(value):
    if sys.version_info[0] >= 3:
        return bytes(value)
    return ''.join(chr(byte) for byte in value)


def byte_value(value):
    return value if isinstance(value, int) else ord(value)


def put_u16(record, offset, value):
    struct.pack_into('<H', record, offset, value)


def put_u32(record, offset, value):
    struct.pack_into('<I', record, offset, value)


def put_u64(record, offset, value):
    struct.pack_into('<Q', record, offset, value)


def put_ascii(record, offset, value, width):
    encoded = value.encode('ascii')
    check(len(encoded) <= width, 'ASCII value exceeds wire field')
    for index in range(width):
        record[offset + index] = byte_value(encoded[index]) if index < len(encoded) else 0


def symbol(channel, stock_index):
    # Keep every symbol globally unique so the shard plan never sees a stock
    # move between exchange channels.
    return '%06d' % (600000 + channel * 100 + stock_index + 1)


def raw_tick(channel, security, sequence, stock_index):
    record = bytearray(72)
    put_u32(record, 0, channel * 10000 + sequence)
    put_u32(record, 4, 99)  # Reserved and intentionally ignored by the decoder.
    record[8] = 0x3e
    put_u64(record, 9, sequence)
    put_u16(record, 17, channel)
    put_ascii(record, 21, security, 8)
    put_u32(record, 30, 9300000)  # 09:30:00.00, HHMMSScc.
    record[34] = ord('A')
    put_u64(record, 35, channel * 1000000 + stock_index + 1)
    put_u64(record, 43, 0)  # Buy adds have no sell-order counterpart.
    put_u32(record, 51, 10000)
    put_u64(record, 55, 1000)
    put_u64(record, 63, 10000000)
    record[71] = 0  # sse_hpf_tick_merge encodes buy side as numeric zero.
    return record


def raw_snapshot(channel, security, sequence):
    record = bytearray(440)
    put_u32(record, 0, channel * 10000 + sequence)
    record[8] = 0x27
    put_u32(record, 21, sequence)
    put_u32(record, 26, 93000)  # 09:30:00, HHMMSS.
    put_ascii(record, 30, security, 8)

    # Give all quote/statistic bytes a non-zero value, then install values
    # matching the decoder's compact snapshot ABI.
    for index in range(42, len(record)):
        record[index] = (index + channel + sequence) % 254 + 1
    put_u32(record, 42, 10000)
    put_u32(record, 46, 10000)
    put_u32(record, 50, 10001)
    put_u32(record, 54, 9999)
    put_u32(record, 58, 10000)
    put_u64(record, 74, 1000000)
    put_u64(record, 82, 1000000000)
    for level in range(5):
        bid = 124 + level * 16
        ask = 124 + (10 + level) * 16
        put_u32(record, bid, 10000 - level)
        put_u64(record, bid + 4, 1000000)
        put_u32(record, ask, 10000 + level)
        put_u64(record, ask + 4, 1000000)
    return record


def reserve_port():
    reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        reservation.bind(('127.0.0.1', 0))
        return reservation.getsockname()[1]
    finally:
        reservation.close()


def read_text(path):
    try:
        with open(path, 'rb') as stream:
            value = stream.read()
        return value.decode('utf-8', 'replace')
    except (IOError, OSError):
        return ''


def wait_for_file(path, process, timeout, log_path):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return
        if process.poll() is not None:
            raise AssertionError('pipeline exited before %s: %s\n%s' %
                                 (os.path.basename(path), process.returncode,
                                  read_text(log_path)))
        time.sleep(0.02)
    raise AssertionError('timed out waiting for %s\n%s' %
                         (path, read_text(log_path)))


def wait_for_process(process, timeout, log_path):
    deadline = time.time() + timeout
    while process.poll() is None and time.time() < deadline:
        time.sleep(0.02)
    if process.poll() is None:
        process.terminate()
        terminate_deadline = time.time() + 1.0
        while process.poll() is None and time.time() < terminate_deadline:
            time.sleep(0.02)
        if process.poll() is None:
            process.kill()
        raise AssertionError('pipeline timeout\n%s' % read_text(log_path))
    return process.returncode


def expected_stocks():
    result = {}
    for channel in CHANNELS:
        for stock_index in range(STOCKS_PER_CHANNEL):
            result[symbol(channel, stock_index)] = (channel, stock_index + 1)
    return result


def validate_affinity(report, expected_roles):
    rows = report.get('affinity')
    check(isinstance(rows, list) and len(rows) == len(expected_roles),
          'wrong affinity role count: %r' % rows)
    roles = set()
    l3s = []
    for row in rows:
        check(isinstance(row, dict), 'invalid affinity row')
        roles.add(row.get('role'))
        l3s.append(row.get('l3'))
        check(row.get('l3'), 'affinity row has no L3 domain: %r' % row)
    check(roles == set(expected_roles), 'wrong affinity roles: %r' % rows)
    check(len(set(l3s)) == len(l3s), 'pipeline roles share an L3 domain: %r' % rows)


def normalize_stocks(report, expected):
    rows = report.get('stocks')
    check(isinstance(rows, list) and len(rows) == len(expected),
          'wrong stock report count: %r' % rows)
    normalized = {}
    for row in rows:
        check(isinstance(row, dict), 'invalid stock report row')
        name = row.get('symbol')
        check(name in expected and name not in normalized,
              'unexpected or duplicate stock report: %r' % row)
        channel, sequence = expected[name]
        check(row.get('channel_no') == channel, 'stock channel changed: %r' % row)
        check(row.get('ticks') == 1 and row.get('snapshots') == 1,
              'stock event counts changed: %r' % row)
        check(row.get('rejected') == 0, 'stock event was rejected: %r' % row)
        check(row.get('live_orders') == 1, 'stock add was not retained: %r' % row)
        check(row.get('last_tick_index') == sequence,
              'stock tick sequence changed: %r' % row)
        check(isinstance(row.get('shard'), int) and 0 <= row['shard'] < SHARD_COUNT,
              'stock shard is invalid: %r' % row)
        normalized[name] = (row['channel_no'], row['shard'], row['ticks'],
                            row['snapshots'], row['rejected'], row['live_orders'],
                            row['last_tick_index'])
    check(set(normalized) == set(expected), 'stock set changed')
    return normalized


def validate_report(report, mode, expected):
    check(report.get('mode') == mode, 'wrong pipeline mode: %r' % report)
    check(report.get('input_ok') is True, 'input failed: %r' % report.get('input_error'))
    check(report.get('pipeline_error') in ('', None),
          'pipeline error: %r' % report.get('pipeline_error'))
    check(report.get('journal_error') in ('', None),
          'journal error: %r' % report.get('journal_error'))
    check(report.get('malformed') == 0 and report.get('filtered') == 0,
          'input records were malformed or filtered: %r' % report)
    zero_keys = ('duplicates', 'late_ticks', 'ingress_overflow', 'journal_overflow',
                 'journal_errors', 'pending_overflow', 'routing_errors')
    for key in zero_keys:
        check(report.get(key) == 0, '%s is non-zero: %r' % (key, report.get(key)))
    check(report.get('pending_snapshots') == 0, 'pending snapshots remain')
    check(report.get('events') == EVENT_COUNT, 'wrong event count: %r' % report)
    check(report.get('submitted') == EVENT_COUNT, 'wrong submitted count: %r' % report)
    check(report.get('compute_healthy') is True and report.get('journal_healthy') is True,
          'pipeline health failed: %r' % report)
    check(report.get('journal_written') == (EVENT_COUNT if mode == 'live' else 0),
          'wrong journal count: %r' % report.get('journal_written'))

    counts = report.get('channel_stock_counts')
    check(isinstance(counts, list) and len(counts) == len(CHANNELS),
          'wrong channel count rows: %r' % counts)
    for row in counts:
        check(isinstance(row, list) and len(row) == SHARD_COUNT,
              'wrong channel count shape: %r' % counts)
        check(sum(row) == STOCKS_PER_CHANNEL and max(row) - min(row) <= 1,
              'channel stocks are not balanced: %r' % counts)

    check(sum(report.get('shard_consumed', [])) == EVENT_COUNT,
          'wrong shard consumption count')
    check(all(value == 0 for value in report.get('shard_overflow', [])),
          'shard queue overflow: %r' % report.get('shard_overflow'))
    check(all(value == 0 for value in report.get('shard_errors', [])),
          'shard callback error: %r' % report.get('shard_errors'))
    return normalize_stocks(report, expected)


def make_config(root, port):
    journal = os.path.join(root, 'journal')
    if not os.path.isdir(journal):
        os.makedirs(journal)
    return {
        'market': 'SH',
        'trading_day': TRADING_DAY,
        'shard_count': SHARD_COUNT,
        'journal_directory': journal,
        'journal_segment_bytes': 16384,
        'journal_min_free_bytes': 0,
        'ingress_capacity': 4096,
        'shard_capacity': 4096,
        'journal_capacity': 4096,
        'pending_snapshot_capacity': 4096,
        'duration_ms': DURATION_MS,
        'channels': [{
            'name': 'feed0',
            'group': '127.0.0.1',
            'port': port,
            'interface_ip': '127.0.0.1'
        }]
    }


def send_feed(port):
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # Send snapshots first, including the first stock before any tick, so
        # the pipeline must buffer and later route them from the first tick.
        for channel in CHANNELS:
            for stock_index in range(STOCKS_PER_CHANNEL):
                name = symbol(channel, stock_index)
                sender.sendto(packet_bytes(raw_snapshot(channel, name, stock_index + 1)),
                               ('127.0.0.1', port))
        time.sleep(0.05)
        for channel in CHANNELS:
            for stock_index in range(STOCKS_PER_CHANNEL):
                name = symbol(channel, stock_index)
                sender.sendto(packet_bytes(raw_tick(channel, name, stock_index + 1,
                                                    stock_index)),
                               ('127.0.0.1', port))
    finally:
        sender.close()


def load_json(path):
    with open(path, 'r') as stream:
        return json.load(stream)


def main():
    check(len(sys.argv) == 2, 'usage: sse_event_pipeline_integration_test.py BINARY')
    binary = os.path.abspath(sys.argv[1])
    check(os.path.isfile(binary), 'pipeline binary does not exist: %s' % binary)
    root = tempfile.mkdtemp(prefix='sse_event_pipeline_test_')
    processes = []
    logs = []
    try:
        expected = expected_stocks()
        port = reserve_port()
        config = make_config(root, port)
        config_path = os.path.join(root, 'pipeline.json')
        with open(config_path, 'w') as stream:
            json.dump(config, stream, sort_keys=True, indent=2)
            stream.write('\n')

        live_log_path = os.path.join(root, 'live.log')
        live_log = open(live_log_path, 'wb')
        logs.append(live_log)
        live_process = subprocess.Popen([binary, config_path], stdout=live_log,
                                        stderr=subprocess.STDOUT)
        processes.append(live_process)
        manifest_path = os.path.join(config['journal_directory'], 'pipeline_manifest.json')
        wait_for_file(manifest_path, live_process, 5.0, live_log_path)
        manifest = load_json(manifest_path)
        check(manifest.get('trading_day') == TRADING_DAY and
              manifest.get('event_version') == 1 and
              manifest.get('event_bytes') == 504 and
              manifest.get('shard_count') == SHARD_COUNT,
              'invalid pipeline manifest: %r' % manifest)
        time.sleep(0.20)
        send_feed(port)
        check(wait_for_process(live_process, 8.0, live_log_path) == 0,
              'live pipeline failed:\n%s' % read_text(live_log_path))
        live_metrics_path = os.path.join(config['journal_directory'], 'metrics.json')
        check(os.path.exists(live_metrics_path), 'live metrics were not written')
        live_report = load_json(live_metrics_path)
        validate_affinity(live_report,
                          ('feed0', 'dispatcher', 'journal', 'shard0', 'shard1', 'shard2'))
        live_stocks = validate_report(live_report, 'live', expected)
        journal_files = [name for name in os.listdir(config['journal_directory'])
                         if name.startswith('sse_') and name.endswith('.szej')]
        check(journal_files, 'live capture did not write a journal segment')

        replay_log_path = os.path.join(root, 'replay.log')
        replay_log = open(replay_log_path, 'wb')
        logs.append(replay_log)
        replay_process = subprocess.Popen([binary, config_path, '--replay'],
                                          stdout=replay_log, stderr=subprocess.STDOUT)
        processes.append(replay_process)
        check(wait_for_process(replay_process, 8.0, replay_log_path) == 0,
              'replay pipeline failed:\n%s' % read_text(replay_log_path))
        replay_metrics_path = os.path.join(config['journal_directory'],
                                           'replay_metrics.json')
        check(os.path.exists(replay_metrics_path), 'replay metrics were not written')
        replay_report = load_json(replay_metrics_path)
        validate_affinity(replay_report,
                          ('input', 'dispatcher', 'journal', 'shard0', 'shard1', 'shard2'))
        replay_stocks = validate_report(replay_report, 'replay', expected)
        check(replay_stocks == live_stocks, 'replay stock routes/counts differ from live')
        check(replay_report.get('channel_stock_counts') ==
              live_report.get('channel_stock_counts'),
              'replay channel stock counts differ from live')
        print('sse_event_pipeline_integration_test: PASS')
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                deadline = time.time() + 1.0
                while process.poll() is None and time.time() < deadline:
                    time.sleep(0.02)
                if process.poll() is None:
                    process.kill()
        for process in processes:
            if process.poll() is None:
                process.wait()
        for log in logs:
            log.close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    main()
