#!/usr/bin/env python
"""Exercise launcher ownership and fresh epochs using only loopback UDP."""
from __future__ import print_function

import argparse
import errno
import fcntl
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


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def read(path):
    with open(path, 'rb') as source:
        return source.read()


def reserve_port():
    handle = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        handle.bind(('127.0.0.1', 0))
        return handle.getsockname()[1]
    finally:
        handle.close()


def packet(width, marker, tag):
    value = bytearray([tag] * width)
    value[8] = marker
    struct.pack_into('<I', value, 0, tag)
    if sys.version_info[0] >= 3:
        return bytes(value)
    return ''.join(chr(byte) for byte in value)


def await_exit(process, timeout=10):
    until = time.time() + timeout
    while process.poll() is None and time.time() < until:
        time.sleep(0.02)
    check(process.poll() is not None, 'test capture failed to stop')
    return process.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launcher', required=True)
    parser.add_argument('--binary', required=True)
    args = parser.parse_args()
    binary = os.path.realpath(args.binary)
    launcher = os.path.realpath(args.launcher)
    root = tempfile.mkdtemp(prefix='sse-journal-launcher-test-')
    runtime = os.path.join(root, 'runtime')
    source_path = os.path.join(root, 'source.json')
    ports = [reserve_port(), reserve_port()]
    while ports[1] == ports[0]:
        ports[1] = reserve_port()
    config = {
        'schema_version': 1, 'payload_format': 'sse-stream-v2',
        'source_id': 89, 'journal_prefix': 'sse',
        'segment_bytes': 1048576, 'min_free_bytes_after_allocate': 0,
        'ring_capacity': 1024, 'journal_queue_capacity': 4096,
        'queue_capacity': 4096, 'max_datagram_bytes': 8192,
        'receive_batch_size': 64, 'receive_buffer_bytes': 1048576,
        'idle_gap_ns': 100000, 'receive_cpu': -1, 'dispatch_cpu': -1,
        'journal_cpu': -1, 'prediction_cpu': -1, 'flush_interval_ms': 5,
        'channels': [
            {'name': 'test_tick', 'group': '127.0.0.1',
             'interface_ip': '127.0.0.1', 'port': ports[0]},
            {'name': 'test_snapshot', 'group': '127.0.0.1',
             'interface_ip': '127.0.0.1', 'port': ports[1]}]}
    with open(source_path, 'w') as output:
        json.dump(config, output)
    original_config = read(source_path)
    command = [sys.executable, launcher, '--config', source_path,
               '--binary', binary, '--runtime-root', runtime]
    processes = []
    rings = []
    epochs = []
    saved_segments = {}
    generations = set()
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for restart in range(3):
            transcript = open(os.path.join(root, 'launcher-%s.log' % restart), 'wb')
            process = subprocess.Popen(command, stdout=transcript, stderr=transcript)
            transcript.close()
            processes.append(process)
            deadline = time.time() + 10
            epoch = None
            while time.time() < deadline:
                check(process.poll() is None, 'launcher/capture exited before readiness: ' +
                      read(os.path.join(root, 'launcher-%s.log' % restart)).decode('utf-8', 'replace'))
                active = os.path.join(runtime, 'active_capture')
                if os.path.islink(active):
                    candidate = os.path.realpath(active)
                    stderr = os.path.join(candidate, 'capture.stderr')
                    if candidate not in epochs and os.path.isfile(stderr):
                        if b'Shanghai journal capture ready' in read(stderr):
                            epoch = candidate
                            break
                time.sleep(0.02)
            check(epoch is not None, 'new capture epoch did not become ready')
            check(os.readlink('/proc/%s/exe' % process.pid) == binary,
                  'launcher did not exec the real capture binary')
            epochs.append(epoch)
            with open(os.path.join(epoch, 'capture.json')) as source:
                launched = json.load(source)
            ring = launched['shm_path']
            rings.append(ring)
            check(launched['generation'] not in generations, 'restart reused generation')
            generations.add(launched['generation'])
            check(os.path.isdir(launched['journal_directory']), 'journal directory missing')
            check(os.path.isfile(ring), 'SHM ring missing')
            check(os.path.realpath(os.path.join(os.path.dirname(epoch), 'current')) == epoch,
                  'day current pointer does not match active capture')
            check(launched['duration_ms'] == 0, 'launcher failed to clear finite duration')

            # Prove the fd survived exec rather than just the launcher holding
            # a lock briefly. The running executable is already the C++ binary.
            lock_path = os.path.join(runtime, 'journal_capture.lock')
            fd_directory = '/proc/%s/fd' % process.pid
            inherited = False
            for descriptor in os.listdir(fd_directory):
                try:
                    inherited = inherited or os.readlink(os.path.join(fd_directory, descriptor)) == lock_path
                except OSError:
                    pass
            check(inherited, 'capture did not inherit the runtime lock descriptor')
            with open(lock_path, 'a') as lock:
                blocked = False
                try:
                    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                except IOError as error:
                    check(error.errno in (errno.EAGAIN, errno.EACCES), 'unexpected flock failure')
                    blocked = True
                check(blocked, 'second process can acquire running capture lock')

            duplicate_log = open(os.path.join(root, 'duplicate-%s.log' % restart), 'wb')
            duplicate = subprocess.Popen(command, stdout=duplicate_log, stderr=duplicate_log)
            duplicate_log.close()
            processes.append(duplicate)
            check(await_exit(duplicate) != 0, 'duplicate launch unexpectedly succeeded')
            check(os.path.realpath(os.path.join(runtime, 'active_capture')) == epoch,
                  'duplicate launch replaced active capture')
            check(len(glob.glob(os.path.join(runtime, '*', 'capture-*'))) == len(epochs),
                  'duplicate launch allocated a new epoch before acquiring lock')

            tick = packet(72, 0x3e, 51 + restart)
            snapshot = packet(440, 0x27, 81 + restart)
            sender.sendto(tick, ('127.0.0.1', ports[0]))
            sender.sendto(snapshot, ('127.0.0.1', ports[1]))
            deadline = time.time() + 5
            segments = []
            while time.time() < deadline:
                segments = glob.glob(os.path.join(launched['journal_directory'], '*.szej'))
                content = b''.join(read(path) for path in segments)
                if tick in content and snapshot in content:
                    break
                check(process.poll() is None, 'capture failed during test traffic')
                time.sleep(0.02)
            check(tick in content and snapshot in content, 'journal did not record both loopback channels')
            process.send_signal(signal.SIGKILL if restart == 1 else signal.SIGTERM)
            code = await_exit(process)
            check(code == (-signal.SIGKILL if restart == 1 else 0), 'unexpected stop result')
            for path, previous in saved_segments.items():
                check(read(path) == previous, 'restart changed previous epoch journal: ' + path)
            for path in segments:
                saved_segments[path] = read(path)
            if restart:
                check(not os.path.exists(rings[-2]), 'old disposable SHM name was not removed')
            check(read(source_path) == original_config, 'launcher changed source configuration')
        check(len(set(rings)) == 3, 'restart reused SHM path')
        check(len(set(epochs)) == 3, 'restart reused journal epoch')
        print('sse_journal_launcher_test: PASS (graceful + crash restart; inherited lock; both channels)')
    finally:
        sender.close()
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait()
        # Only paths read from this test's own freshly allocated configurations.
        for ring in rings:
            if os.path.dirname(ring) == '/dev/shm' and os.path.basename(ring).startswith('usagi-sse-journal-capture-'):
                try:
                    os.unlink(ring)
                except OSError as error:
                    if error.errno != errno.ENOENT:
                        raise
        shutil.rmtree(root)


if __name__ == '__main__':
    main()
