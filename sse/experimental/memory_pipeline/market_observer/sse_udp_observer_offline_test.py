#!/usr/bin/env python
from __future__ import print_function
import binascii
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

binary = sys.argv[1]
ports = (39105, 39106)


def payload(values):
    return bytearray(values)


def hex_string(value):
    return binascii.hexlify(value).decode('ascii')


with tempfile.NamedTemporaryFile(prefix="sse_udp_observer_", suffix=".jsonl", delete=False) as output:
    path = output.name
try:
    process = subprocess.Popen([
        binary, path,
        "offline_a", "127.0.0.1", str(ports[0]),
        "offline_b", "127.0.0.1", str(ports[1]),
        "--interface-ip", "127.0.0.1",
        "--duration-ms", "700"], stderr=subprocess.PIPE)
    time.sleep(0.15)
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payloads = (payload(range(32)), payload(range(32, 64)))
    sender.sendto(payloads[0], ("127.0.0.1", ports[0]))
    sender.sendto(payloads[1], ("127.0.0.1", ports[1]))
    sender.close()
    deadline = time.time() + 3.0
    while process.poll() is None and time.time() < deadline:
        time.sleep(0.02)
    if process.poll() is None:
        process.terminate()
        process.wait()
        raise AssertionError("observer timed out")
    if process.returncode != 0:
        raise AssertionError("observer exited unsuccessfully")
    rows = [json.loads(line) for line in open(path) if line.strip()]
    assert len(rows) == 2, rows
    rows = {row["channel"]: row for row in rows}
    assert set(rows) == {"offline_a", "offline_b"}, rows
    for index, channel in enumerate(("offline_a", "offline_b")):
        assert rows[channel]["ts_ns"] > 0, rows[channel]
        assert rows[channel]["monotonic_ns"] > 0, rows[channel]
        assert rows[channel]["length"] == len(payloads[index]), rows[channel]
        assert rows[channel]["prefix_hex"] == hex_string(payloads[index]), rows[channel]
    print("sse_udp_observer_offline_test: PASS")
finally:
    try:
        os.unlink(path)
    except OSError:
        pass
