#!/usr/bin/env python3
import json
import binascii
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

binary = sys.argv[1]
reservations = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2)]
for reservation in reservations:
    reservation.bind(("127.0.0.1", 0))
ports = tuple(reservation.getsockname()[1] for reservation in reservations)
for reservation in reservations:
    reservation.close()


def wait_for_sockets(process):
    deadline = time.time() + 3
    while process.poll() is None and time.time() < deadline:
        inodes = set()
        for name in os.listdir("/proc/{}/fd".format(process.pid)):
            try:
                target = os.readlink("/proc/{}/fd/{}".format(process.pid, name))
                if target.startswith("socket:["):
                    inodes.add(target[8:-1])
            except OSError:
                pass
        with open("/proc/{}/net/udp".format(process.pid)) as sockets:
            bound = set(int(fields[1].split(":")[1], 16)
                        for fields in (line.split() for line in sockets)
                        if len(fields) > 9 and fields[9] in inodes)
        if set(ports).issubset(bound):
            return
        time.sleep(0.01)
    raise AssertionError("observer sockets did not become ready")


process = None
with tempfile.NamedTemporaryFile(prefix="sse_udp_observer_", suffix=".jsonl", delete=False) as output:
    path = output.name
try:
    process = subprocess.Popen([
        binary, path,
        "offline_a", "127.0.0.1", str(ports[0]),
        "offline_b", "127.0.0.1", str(ports[1]),
        "--interface-ip", "127.0.0.1",
        "--cpu-list", "-1,-1",
        "--duration-ms", "700"], stderr=subprocess.PIPE)
    wait_for_sockets(process)
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payloads = (struct.pack("32B", *range(32)), struct.pack("32B", *range(32, 64)))
    sender.sendto(payloads[0], ("127.0.0.1", ports[0]))
    sender.sendto(payloads[1], ("127.0.0.1", ports[1]))
    sender.close()
    deadline = time.time() + 3
    while process.poll() is None and time.time() < deadline:
        time.sleep(0.01)
    if process.poll() is None:
        process.kill()
        process.wait()
        raise AssertionError("observer did not exit before deadline")
    if process.returncode != 0:
        raise AssertionError("observer exited unsuccessfully: " + process.stderr.read().decode("utf-8", "replace"))
    rows = [json.loads(line) for line in open(path) if line.strip()]
    assert len(rows) == 2, rows
    rows = {row["channel"]: row for row in rows}
    assert set(rows) == {"offline_a", "offline_b"}, rows
    for index, channel in enumerate(("offline_a", "offline_b")):
        assert rows[channel]["ts_ns"] > 0, rows[channel]
        assert rows[channel]["monotonic_ns"] > 0, rows[channel]
        assert rows[channel]["length"] == len(payloads[index]), rows[channel]
        assert rows[channel]["prefix_hex"] == binascii.hexlify(payloads[index]).decode("ascii"), rows[channel]
        assert rows[channel]["source_ip"] == "127.0.0.1", rows[channel]
    print("sse_udp_observer_offline_test: PASS")
finally:
    if process is not None and process.poll() is None:
        process.kill()
        process.wait()
    try:
        os.unlink(path)
    except OSError:
        pass
