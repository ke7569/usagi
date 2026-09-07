#!/usr/bin/env python3
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

binary = sys.argv[1]
ports = (39105, 39106)
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
    payloads = (bytes(range(32)), bytes(range(32, 64)))
    sender.sendto(payloads[0], ("127.0.0.1", ports[0]))
    sender.sendto(payloads[1], ("127.0.0.1", ports[1]))
    sender.close()
    if process.wait(timeout=3) != 0:
        raise AssertionError("observer exited unsuccessfully")
    rows = [json.loads(line) for line in open(path) if line.strip()]
    assert len(rows) == 2, rows
    rows = {row["channel"]: row for row in rows}
    assert set(rows) == {"offline_a", "offline_b"}, rows
    for index, channel in enumerate(("offline_a", "offline_b")):
        assert rows[channel]["ts_ns"] > 0, rows[channel]
        assert rows[channel]["monotonic_ns"] > 0, rows[channel]
        assert rows[channel]["length"] == len(payloads[index]), rows[channel]
        assert rows[channel]["prefix_hex"] == payloads[index].hex(), rows[channel]
    print("sse_udp_observer_offline_test: PASS")
finally:
    try:
        os.unlink(path)
    except OSError:
        pass
