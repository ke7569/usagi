#!/usr/bin/env python
from __future__ import print_function

"""End-to-end SSE journal/SHM handoff test.

The capture and prediction binaries are deliberately separate processes.  The
test sends only loopback UDP and uses the factors-only processing profile, so
it does not require production model artifacts or a trading connection.
"""

import glob
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


PAYLOAD_MAGIC = 0x31534853
RECORD_MAGIC = 0x31525a53
COMMIT_MAGIC = 0x3154494d4d4f4353
PAYLOAD_HEADER_BYTES = 48
JOURNAL_HEADER_BYTES = 4096
JOURNAL_RECORD_HEADER_BYTES = 72
JOURNAL_TRAILER_BYTES = 16


class SkipTest(Exception):
    pass


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
    check(len(encoded) <= width, 'wire ASCII field is too long')
    for index in range(width):
        record[offset + index] = (byte_value(encoded[index])
                                  if index < len(encoded) else 0)


def raw_tick(wire_sequence, tick_index, kind, buy_order, sell_order,
             quantity=1000, security='600000'):
    record = bytearray(72)
    put_u32(record, 0, wire_sequence)
    put_u32(record, 4, 99)  # Reserved bytes must be ignored by the decoder.
    record[8] = 0x3e
    put_u64(record, 9, tick_index)
    put_u16(record, 17, 7)  # Exchange channel number, independent of UDP id.
    put_ascii(record, 21, security, 8)
    put_u32(record, 30, 9300000)  # 09:30:00.00, HHMMSScc.
    record[34] = byte_value(kind)
    put_u64(record, 35, buy_order)
    put_u64(record, 43, sell_order)
    put_u32(record, 51, 10000)
    put_u64(record, 55, quantity)
    put_u64(record, 63, quantity * 10000)
    # The SSE decoder uses numeric wire side: zero buy, one sell.
    record[71] = 0 if buy_order else 1
    return packet_bytes(record)


def raw_snapshot(wire_sequence, security='600000'):
    record = bytearray(440)
    put_u32(record, 0, wire_sequence)
    record[8] = 0x27
    put_u32(record, 21, wire_sequence)
    put_u32(record, 26, 93000)  # 09:30:00, HHMMSS.
    put_ascii(record, 30, security, 8)
    for index in range(42, len(record)):
        record[index] = (index + wire_sequence) % 254 + 1
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
    return packet_bytes(record)


def reserve_port():
    handle = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        handle.bind(('127.0.0.1', 0))
        return handle.getsockname()[1]
    finally:
        handle.close()


def write_json(path, value):
    with open(path, 'w') as output:
        json.dump(value, output, sort_keys=True, indent=2)
        output.write('\n')


def boot_id():
    try:
        with open('/proc/sys/kernel/random/boot_id', 'r') as source:
            value = source.read().strip()
    except (IOError, OSError):
        raise SkipTest('Linux boot identity is unavailable')
    if not value:
        raise SkipTest('Linux boot identity is unavailable')
    return value


def make_config(root, ports, day):
    journal = os.path.join(root, 'journal')
    os.mkdir(journal)
    generation = ((int(time.time() * 1000000) << 8) ^ os.getpid()) & 0x7fffffffffffffff
    return {
        'schema_version': 1,
        'payload_format': 'sse-stream-v1',
        'trading_day': day,
        'source_id': 89,
        'generation': generation or 1,
        'boot_id': boot_id(),
        'journal_directory': journal,
        'journal_prefix': 'sse',
        'shm_path': os.path.join(root, 'events.shm'),
        'segment_bytes': 1048576,
        'min_free_bytes_after_allocate': 0,
        'ring_capacity': 1024,
        'journal_queue_capacity': 4096,
        'queue_capacity': 4096,
        'max_datagram_bytes': 8192,
        'receive_batch_size': 64,
        'receive_buffer_bytes': 1048576,
        'idle_gap_ns': 100000,
        'receive_cpu': -1,
        'dispatch_cpu': -1,
        'journal_cpu': -1,
        'prediction_cpu': -1,
        'flush_interval_ms': 5,
        'duration_ms': 0,
        'channels': [
            {'name': 'loopback-0', 'group': '127.0.0.1',
             'interface_ip': '127.0.0.1', 'port': ports[0]},
            {'name': 'loopback-1', 'group': '127.0.0.1',
             'interface_ip': '127.0.0.1', 'port': ports[1]},
        ],
    }


def make_profile(path, day):
    write_json(path, {
        'schema_version': 1,
        'market': 'SH',
        'execution': 'disabled',
        'processing_mode': 'factors-only',
        'processing_contract': 'sse-per-instrument-v2',
        'trading_day': day,
        'processing_sha256': '0' * 64,
        'environment': {'mode': 'live', 'clock': 'host', 'execution': 'disabled'},
        'prediction': {},
        'instruments': [{
            'instrument': '600000',
            'trading_date': day,
            'average_amount': 8000000.0,
            'turnover_threshold': 1000.0,
            'free_share': 10000000.0,
            'pre_close': 10.0,
            'upper_limit': 11.0,
            'lower_limit': 9.0,
        }],
    })


def read_text(path):
    try:
        with open(path, 'rb') as source:
            value = source.read()
        return value.decode('utf-8', 'replace')
    except (IOError, OSError):
        return ''


def last_json(path):
    lines = read_text(path).splitlines()
    for line in reversed(lines):
        try:
            value = json.loads(line)
            if isinstance(value, dict):
                return value
        except (TypeError, ValueError):
            continue
    return None


def launch(binary, arguments, root, name):
    stdout_path = os.path.join(root, name + '.stdout')
    stderr_path = os.path.join(root, name + '.stderr')
    stdout = open(stdout_path, 'wb')
    stderr = open(stderr_path, 'wb')
    process = subprocess.Popen([binary] + arguments, stdout=stdout, stderr=stderr)
    return process, stdout, stderr, stdout_path, stderr_path


def wait_for_files(paths, process, timeout, detail_path):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if all(os.path.exists(path) for path in paths):
            return
        if process.poll() is not None:
            detail = read_text(detail_path)
            lowered = detail.lower()
            if any(word in lowered for word in ('l3', 'cpu affinity', 'not enough')):
                raise SkipTest('capture CPU topology cannot provide three L3 domains')
            raise AssertionError('capture exited before startup: %s\n%s' %
                                 (process.returncode, detail))
        time.sleep(0.02)
    raise AssertionError('capture startup timed out\n%s' % read_text(detail_path))


def require_alive(process, timeout, detail_path, name):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if process.poll() is not None:
            detail = read_text(detail_path)
            lowered = detail.lower()
            if any(word in lowered for word in ('l3', 'cpu affinity', 'not enough')):
                raise SkipTest('%s CPU topology is unavailable' % name)
            raise AssertionError('%s exited unexpectedly: %s\n%s' %
                                 (name, process.returncode, detail))
        time.sleep(0.02)


def stop_process(process, stop_signal, timeout, name):
    if process.poll() is None:
        os.kill(process.pid, stop_signal)
    deadline = time.time() + timeout
    while process.poll() is None and time.time() < deadline:
        time.sleep(0.02)
    if process.poll() is None:
        os.kill(process.pid, signal.SIGKILL)
        process.wait()
        raise AssertionError('%s did not stop cleanly' % name)
    return process.returncode


def parse_segment(path):
    try:
        with open(path, 'rb') as source:
            data = source.read()
    except (IOError, OSError):
        return [], None
    if len(data) < JOURNAL_HEADER_BYTES:
        return [], None
    try:
        header = struct.unpack_from('<8s6I7Q2I', data, 0)
    except struct.error:
        return [], None
    journal_magic = b'SZEJRNL1' if sys.version_info[0] >= 3 else 'SZEJRNL1'
    if header[0] != journal_magic:
        return [], None
    published = header[12]
    segment_bytes = header[7]
    if published < JOURNAL_HEADER_BYTES or published > segment_bytes or published > len(data):
        return [], None
    records = []
    offset = JOURNAL_HEADER_BYTES
    while offset + JOURNAL_RECORD_HEADER_BYTES <= published:
        try:
            magic = struct.unpack_from('<I', data, offset)[0]
            total = struct.unpack_from('<I', data, offset + 8)[0]
            payload_bytes = struct.unpack_from('<I', data, offset + 12)[0]
            event_id = struct.unpack_from('<Q', data, offset + 16)[0]
        except struct.error:
            break
        if magic != RECORD_MAGIC or total < JOURNAL_RECORD_HEADER_BYTES + JOURNAL_TRAILER_BYTES:
            break
        if total > published - offset or payload_bytes > total - JOURNAL_RECORD_HEADER_BYTES - JOURNAL_TRAILER_BYTES:
            break
        trailer = offset + total - JOURNAL_TRAILER_BYTES
        try:
            trailer_event, commit = struct.unpack_from('<QQ', data, trailer)
        except struct.error:
            break
        if trailer_event != event_id or commit != COMMIT_MAGIC:
            break
        body = data[offset + JOURNAL_RECORD_HEADER_BYTES:
                    offset + JOURNAL_RECORD_HEADER_BYTES + payload_bytes]
        records.append((event_id, body))
        offset += total
    return records, header[14]


def journal_snapshot(directory):
    records = []
    clean = []
    for path in sorted(glob.glob(os.path.join(directory, 'sse_*.szej'))):
        segment_records, segment_clean = parse_segment(path)
        records.extend(segment_records)
        if segment_clean is not None:
            clean.append(segment_clean)
    records.sort(key=lambda item: item[0])
    datagrams = []
    for unused_event_id, body in records:
        if len(body) < PAYLOAD_HEADER_BYTES:
            continue
        try:
            magic, version, kind = struct.unpack_from('<IHH', body, 0)
            payload_bytes = struct.unpack_from('<I', body, 44)[0]
        except struct.error:
            continue
        if (magic == PAYLOAD_MAGIC and version == 1 and kind == 1 and
                payload_bytes + PAYLOAD_HEADER_BYTES == len(body)):
            datagrams.append(body[PAYLOAD_HEADER_BYTES:])
    return records, datagrams, clean


def wait_for_datagrams(directory, minimum, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        unused_records, datagrams, unused_clean = journal_snapshot(directory)
        if len(datagrams) >= minimum:
            return datagrams
        time.sleep(0.02)
    records, datagrams, unused_clean = journal_snapshot(directory)
    raise AssertionError('journal did not reach %d datagrams (records=%d datagrams=%d)' %
                         (minimum, len(records), len(datagrams)))


def send_round(port, state, include_snapshot):
    packets = []
    wire = state['wire_sequence']
    tick_index = state['tick_index']
    if include_snapshot:
        packets.append(raw_snapshot(wire))
        wire += 1
    buy_order = 1000 + state['round'] * 100 + 1
    sell_order = 2000 + state['round'] * 100 + 1
    packets.append(raw_tick(wire, tick_index, 'A', buy_order, 0))
    wire += 1
    tick_index += 1
    packets.append(raw_tick(wire, tick_index, 'A', 0, sell_order))
    wire += 1
    tick_index += 1
    packets.append(raw_tick(wire, tick_index, 'T', buy_order, sell_order, 100))
    wire += 1
    tick_index += 1
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for packet in packets:
            sender.sendto(packet, ('127.0.0.1', port))
            # Give the receiver a chance to preserve packet order under a busy
            # CI host while keeping the test entirely loopback-local.
            time.sleep(0.002)
    finally:
        sender.close()
    state['wire_sequence'] = wire
    state['tick_index'] = tick_index
    state['round'] += 1
    return packets


def validate_prediction(path, minimum_events):
    result = last_json(path)
    check(result is not None, 'predictor did not emit final JSON: %s' % read_text(path))
    check(result.get('ok') is True, 'predictor failed: %r' % result)
    check(result.get('mode') == 'live', 'restarted predictor did not reach live: %r' % result)
    check(result.get('replayed_events', 0) > 0, 'predictor did not replay journal events')
    check(result.get('live_events', 0) > 0, 'predictor did not consume live SHM events')
    check(result.get('events', 0) >= minimum_events,
          'predictor consumed too few events: %r' % result)
    processing = result.get('processing') or {}
    check(processing.get('samples', 0) > 0,
          'valid SSE input produced no processing samples: %r' % result)
    return result


def validate_capture_status(path):
    result = last_json(path)
    if result is None:
        return
    check(result.get('ok', True) is True, 'capture final status failed: %r' % result)
    rows = result.get('cpu_affinity')
    if rows is None:
        rows = result.get('affinity')
    if rows is not None:
        check(isinstance(rows, list) and len(rows) == 3,
              'capture did not report three CPU roles: %r' % rows)
        domains = []
        for row in rows:
            check(isinstance(row, dict), 'invalid capture CPU role: %r' % row)
            domain = row.get('l3') or row.get('domain')
            check(domain, 'capture CPU role has no L3 domain: %r' % row)
            domains.append(domain)
        check(len(set(domains)) == 3,
              'capture roles share an L3 domain: %r' % rows)


def run(capture_binary, predictor_binary):
    check(os.path.isfile(capture_binary), 'capture binary does not exist: %s' % capture_binary)
    check(os.path.isfile(predictor_binary), 'predictor binary does not exist: %s' % predictor_binary)
    if os.name != 'posix':
        raise SkipTest('journal handoff integration requires POSIX signals and SHM')

    root = tempfile.mkdtemp(prefix='sse-journal-integration-')
    children = []
    handles = []
    capture = None
    predictor_one = None
    predictor_two = None
    try:
        day = int(time.strftime('%Y%m%d'))
        config = make_config(root, (reserve_port(), reserve_port()), day)
        config_path = os.path.join(root, 'transport.json')
        profile_path = os.path.join(root, 'factors-profile.json')
        write_json(config_path, config)
        make_profile(profile_path, day)

        capture = launch(capture_binary, [config_path], root, 'capture')
        children.append(capture[0])
        handles.extend(capture[1:3])
        wait_for_files([config['shm_path'], config['journal_directory']],
                       capture[0], 10.0, capture[4])
        segment_deadline = time.time() + 10.0
        while time.time() < segment_deadline and not glob.glob(
                os.path.join(config['journal_directory'], 'sse_*.szej')):
            require_alive(capture[0], 0.1, capture[4], 'capture')
        check(glob.glob(os.path.join(config['journal_directory'], 'sse_*.szej')),
              'capture did not create a journal segment')
        time.sleep(0.15)

        state = {'round': 0, 'wire_sequence': 1, 'tick_index': 1}
        sent = []
        sent.extend(send_round(config['channels'][0]['port'], state, True))
        wait_for_datagrams(config['journal_directory'], len(sent), 8.0)

        predictor_one = launch(predictor_binary,
                               [config_path, profile_path, '--duration-ms', '0'],
                               root, 'predictor-one')
        children.append(predictor_one[0])
        handles.extend(predictor_one[1:3])
        require_alive(predictor_one[0], 1.0, predictor_one[4], 'predictor-one')

        sent.extend(send_round(config['channels'][0]['port'], state, False))
        wait_for_datagrams(config['journal_directory'], len(sent), 8.0)
        time.sleep(0.35)
        records_before_kill, unused_datagrams, unused_clean = journal_snapshot(
            config['journal_directory'])
        check(predictor_one[0].poll() is None, 'predictor-one stopped before isolation check')
        os.kill(predictor_one[0].pid, signal.SIGKILL)
        predictor_one[0].wait()
        check(predictor_one[0].returncode != 0,
              'SIGKILL did not terminate predictor-one as expected')
        check(capture[0].poll() is None,
              'capture stopped when predictor-one was killed')

        sent.extend(send_round(config['channels'][0]['port'], state, False))
        after_kill = wait_for_datagrams(config['journal_directory'], len(sent), 8.0)
        records_after_kill, unused_datagrams, unused_clean = journal_snapshot(
            config['journal_directory'])
        check(len(records_after_kill) > len(records_before_kill),
              'capture journal did not grow after predictor SIGKILL')

        predictor_two = launch(predictor_binary,
                               [config_path, profile_path, '--duration-ms', '0'],
                               root, 'predictor-two')
        children.append(predictor_two[0])
        handles.extend(predictor_two[1:3])
        require_alive(predictor_two[0], 1.0, predictor_two[4], 'predictor-two')
        time.sleep(0.35)
        records_before_restart_feed, unused_datagrams, unused_clean = journal_snapshot(
            config['journal_directory'])
        sent.extend(send_round(config['channels'][0]['port'], state, False))
        wait_for_datagrams(config['journal_directory'], len(sent), 8.0)
        time.sleep(0.7)
        check(capture[0].poll() is None,
              'capture stopped while restarted predictor was running')
        stop_process(predictor_two[0], signal.SIGTERM, 8.0, 'predictor-two')
        validate_prediction(predictor_two[3], len(records_before_restart_feed))

        stop_process(capture[0], signal.SIGTERM, 10.0, 'capture')
        validate_capture_status(capture[3])
        final_records, final_datagrams, clean_flags = journal_snapshot(
            config['journal_directory'])
        check(final_records, 'capture journal is empty after clean stop')
        check(final_datagrams == sent,
              'journal datagram payloads differ from loopback input')
        event_ids = [item[0] for item in final_records]
        check(event_ids == list(range(1, len(event_ids) + 1)),
              'journal event ids are not contiguous: %r' % event_ids[:16])
        check(clean_flags and clean_flags[-1] == 1,
              'journal did not record a clean producer shutdown')
        print('sse_journal_integration_test: PASS')
    finally:
        for process in children:
            if process.poll() is None:
                try:
                    os.kill(process.pid, signal.SIGTERM)
                except OSError:
                    pass
        deadline = time.time() + 2.0
        for process in children:
            while process.poll() is None and time.time() < deadline:
                time.sleep(0.02)
            if process.poll() is None:
                try:
                    os.kill(process.pid, signal.SIGKILL)
                except OSError:
                    pass
        for process in children:
            try:
                process.wait()
            except OSError:
                pass
        for handle in handles:
            handle.close()
        shutil.rmtree(root, ignore_errors=True)


def main():
    if len(sys.argv) != 3:
        print('usage: sse_journal_integration_test.py CAPTURE_BINARY PREDICTOR_BINARY',
              file=sys.stderr)
        return 2
    try:
        run(os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]))
        return 0
    except SkipTest as error:
        print('SKIP: %s' % error)
        return 0
    except Exception:
        import traceback
        traceback.print_exc()
        return 1


if __name__ == '__main__':
    sys.exit(main())
