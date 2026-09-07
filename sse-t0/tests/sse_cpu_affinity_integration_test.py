#!/usr/bin/env python
from __future__ import print_function
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time


binary = os.path.abspath(sys.argv[1])
root = tempfile.mkdtemp(prefix='sse_affinity_test_')
processes = []
logs = []


def start(name, channels, extra=None, duration=3000):
    path = os.path.join(root, name)
    log = open(path + '.log', 'w')
    logs.append(log)
    command = [binary, path + '.jsonl'] + channels + [
        '--interface-ip', '127.0.0.1', '--duration-ms', str(duration)] + (extra or [])
    process = subprocess.Popen(command, stdout=log, stderr=log)
    processes.append(process)
    return process, path


def wait(process, seconds=6):
    deadline = time.time() + seconds
    while process.poll() is None and time.time() < deadline:
        time.sleep(0.02)
    if process.poll() is None:
        raise AssertionError('observer timeout')
    return process.returncode


def affinity(path, count):
    deadline = time.time() + 2
    while time.time() < deadline:
        text = open(path + '.log').read()
        rows = re.findall(r'SSE affinity channel=(\S+) cpu=(\d+) l3=([\d,]+)', text)
        if len(rows) == count:
            return rows
        time.sleep(0.02)
    raise AssertionError('no CPU plan: ' + text)


try:
    a, path_a = start('a', ['a1', '127.0.0.1', '39251', 'a2', '127.0.0.1', '39252'])
    plan = affinity(path_a, 2)
    assert plan[0][2] != plan[1][2], plan
    time.sleep(0.15)
    masks = []
    for tid in os.listdir('/proc/%d/task' % a.pid):
        text = open('/proc/%d/task/%s/status' % (a.pid, tid)).read()
        masks.append(re.search(r'Cpus_allowed_list:\s*(\S+)', text).group(1))
    assert set(masks) == set(row[1] for row in plan), (masks, plan)
    assert len(masks) == 3, masks
    collision, collision_path = start('collision', ['b', '127.0.0.1', '39253'], ['--cpu', plan[0][1]])
    assert wait(collision) != 0
    assert 'L3' in open(collision_path + '.log').read()
    b, path_b = start('b', ['b', '127.0.0.1', '39254'], duration=1000)
    other = affinity(path_b, 1)
    assert other[0][2] not in set(row[2] for row in plan), (plan, other)
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sender.sendto(b'first', ('127.0.0.1', 39251))
    sender.sendto(b'second', ('127.0.0.1', 39252))
    sender.close()
    assert wait(b) == 0
    assert wait(a) == 0
    rows = [json.loads(line) for line in open(path_a + '.jsonl')]
    assert set(row['channel'] for row in rows) == set(['a1', 'a2']), rows
    assert set(row['prefix_hex'] for row in rows) == set(['6669727374', '7365636f6e64']), rows
    # A released lease must not prevent a later capture using the same CPU.
    reused, unused = start('reuse', ['r', '127.0.0.1', '39255'], ['--cpu', plan[0][1]], duration=100)
    assert wait(reused) == 0
    invalid, unused = start('invalid', ['r', '127.0.0.1', '39256'], ['--cpu', 'bad'])
    assert wait(invalid) == 2
    print('sse_cpu_affinity_integration_test: PASS; plans=%s other=%s masks=%s' % (plan, other, masks))
finally:
    for process in processes:
        if process.poll() is None:
            process.terminate()
        process.wait()
    for log in logs:
        log.close()
    shutil.rmtree(root)
