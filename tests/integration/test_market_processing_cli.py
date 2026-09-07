#!/usr/bin/env python3
"""Offline loopback coverage for the market-processing stream CLIs."""

import datetime
import importlib.util
import json
import os
import socket
import select
import struct
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
TOOLS = os.path.join(ROOT, "tools/config")
sys.path.insert(0, TOOLS)
import prepare_stream_processing as prepare
import unified_config

CLI_ARGS = ()


def port():
    handle = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    handle.bind(("127.0.0.1", 0))
    value = handle.getsockname()[1]
    handle.close()
    return value


def write_json(path, value):
    with open(path, "w") as stream:
        json.dump(value, stream, sort_keys=True)


def put_u32(value, offset, number):
    value[offset:offset + 4] = struct.pack("<I", number)


def put_u64(value, offset, number):
    value[offset:offset + 8] = struct.pack("<Q", number)


def put_ascii(value, offset, text, width):
    encoded = text.encode("ascii")
    value[offset:offset + width] = encoded + b" " * (width - len(encoded))


def sze_time(seconds):
    value = datetime.datetime(2026, 9, 4, 9, 30) + datetime.timedelta(seconds=seconds)
    return int(value.strftime("%Y%m%d%H%M%S000"))


def sze_head(message_type, sequence, application, symbol="000001", seconds=0):
    return struct.pack("<IIBBB9sBQHQI", sequence, 0, message_type, 1, 0,
                       symbol.encode("ascii") + b"\0" * (9 - len(symbol)), 101,
                       sze_time(seconds), 7, application, 1)


def sze_order(sequence, application, buy, seconds):
    return sze_head(23, sequence, application, seconds=seconds) + struct.pack(
        "<IQcc15s", 100000, 10000, b"1" if buy else b"2", b"2", b"\0" * 15)


def sze_execution(sequence, application, buy_id, sell_id, seconds):
    return sze_head(24, sequence, application, seconds=seconds) + struct.pack(
        "<qqIqc", buy_id, sell_id, 100000, 10000, b"F")


def sze_packets():
    result = []
    sequence = 1
    application = 1
    for pair in range(333):
        seconds = pair * 40
        buy_id = application
        result.append(sze_order(sequence, application, True, seconds))
        sequence += 1
        application += 1
        sell_id = application
        result.append(sze_order(sequence, application, False, seconds))
        sequence += 1
        application += 1
        result.append(sze_execution(sequence, application, buy_id, sell_id, seconds + 31))
        sequence += 1
        application += 1
    result.append(sze_order(sequence, application, True, 333 * 40))
    return result


def sse_tick(sequence, index, event_type, side, time_raw, buy_id, sell_id,
             quantity=1000000):
    value = bytearray(72)
    value[8] = 0x3E
    put_u32(value, 0, sequence)
    put_u64(value, 9, index)
    value[17:19] = struct.pack("<H", 7)
    put_ascii(value, 21, "600000", 8)
    put_u32(value, 30, time_raw)
    value[34] = ord(event_type)
    put_u64(value, 35, buy_id)
    put_u64(value, 43, sell_id)
    put_u32(value, 51, 10000)
    put_u64(value, 55, quantity)
    value[71] = side
    return bytes(value)


def sse_packets():
    first = sse_tick(1, 1, "A", 0, 9300000, 1001, 0)
    second = sse_tick(2, 2, "A", 1, 9300000, 0, 2001)
    return [first + second,
            sse_tick(3, 3, "T", 0, 9300100, 1001, 2001, 100000),
            sse_tick(4, 4, "T", 0, 9300200, 1001, 2001, 100000)]


class MarketProcessingCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(CLI_ARGS) != 2:
            raise unittest.SkipTest("usage: test_market_processing_cli.py SZE_BINARY SSE_BINARY")
        cls.sze_binary, cls.sse_binary = CLI_ARGS

    def runtime(self, market):
        if market == "SZ":
            symbol = "000001.SZ"
        else:
            symbol = "600000.SH"
        runtime = {
            "market": market,
            "strategy_name": "stream-cli-test",
            "trading_day": 20260904,
            "model_path": "/absent-fixtures/model.bin",
            "global_params": {"offset": 1.0, "position_limit": 1.0},
            "ins_params": {symbol: {
                "Date": 20260904,
                "Close": 10.0,
                "HistoryAmount": 8000000.0,
                "FreeShare": 10000000.0,
                "HpUpperPrice": 11.0,
                "HpLowerPrice": 9.0,
                "HistoryVolatility20d": 0.02,
                "static_position": 0,
                "last_position": 0,
            }},
        }
        if market == "SZ":
            runtime["sz_orderbook_mode"] = "hp-realtime"
        else:
            runtime.update({
                "sh_orderbook_mode": "complete-orderbook-sh",
                "model_type": "sse_hybrid_native",
                "snapshot_baseline_model_path": "/absent-fixtures/base.bin",
                "snapshot_baseline_scaler_path": "/absent-fixtures/base.json",
                "snapshot_auction59_model_path": "/absent-fixtures/auction.bin",
                "snapshot_auction59_scaler_path": "/absent-fixtures/auction.json",
            })
        return runtime

    def bound_profile(self, directory, market, mode="live", factors_only=True):
        config = unified_config.migrate_runtime(self.runtime(market), "CLI-TEST")
        if mode == "live":
            bound = unified_config.bind_environment(config, "live")
        else:
            bound = unified_config.bind_environment(
                config, "replay", directory, "t0md-v1", "recorded-receive")
        profile = prepare.make_profile(bound, factors_only=factors_only)
        path = os.path.join(os.path.dirname(directory), market + "-profile.json")
        write_json(path, profile)
        return path, bound

    def stream_config(self, recording, stream_port, idle_gap=0):
        return {
            "channels": [{"name": "loopback", "group": "127.0.0.1",
                          "interface_ip": "127.0.0.1", "port": stream_port}],
            "recording_directory": recording,
            "recording_required": True,
            "duration_ms": 500,
            "queue_capacity": 4096,
            "max_datagram_bytes": 1024,
            "receive_batch_size": 64,
            "receive_buffer_bytes": 1048576,
            "idle_gap_ns": idle_gap,
            "segment_bytes": 32768,
            "flush_interval_ms": 5,
            "receive_cpu": -1,
            "dispatch_cpu": -1,
            "writer_cpu": -1,
        }

    def run_cli(self, binary, action, stream_or_recording, profile):
        return subprocess.run(
            [binary, action, stream_or_recording, profile],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)

    def capture_replay(self, market, packets, idle_gap=0):
        binary = self.sze_binary if market == "SZ" else self.sse_binary
        with tempfile.TemporaryDirectory(prefix="market-cli-") as directory:
            recording = os.path.join(directory, "recording")
            profile, bound = self.bound_profile(recording, market)
            stream_path = os.path.join(directory, "stream.json")
            stream_port = port()
            write_json(stream_path, self.stream_config(recording, stream_port, idle_gap))
            process = subprocess.Popen(
                [binary, "capture", stream_path, profile],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            ready = False
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline and process.poll() is None:
                readable, _, _ = select.select([process.stderr], [], [], 0.1)
                if readable and b"market-data stream ready" in process.stderr.readline():
                    ready = True
                    break
            if not ready:
                process.kill()
                process.communicate()
                self.fail("market processing receiver did not become ready")
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                for packet in packets:
                    sender.sendto(packet, ("127.0.0.1", stream_port))
                    time.sleep(0.0001)
            finally:
                sender.close()
            stdout, stderr = process.communicate(timeout=5)
            self.assertEqual(0, process.returncode, stderr.decode("utf-8", "replace"))
            capture = json.loads(stdout.decode().splitlines()[-1])
            self.assertTrue(capture["ok"], stderr.decode("utf-8", "replace"))
            self.assertGreater(capture["processing"]["samples"], 0)
            self.assertEqual(0, capture["processing"]["predictions"])

            replay_profile, _ = self.bound_profile(recording, market, mode="replay")
            replay = self.run_cli(binary, "replay", recording, replay_profile)
            self.assertEqual(0, replay.returncode, replay.stderr.decode("utf-8", "replace"))
            replay_result = json.loads(replay.stdout.decode().splitlines()[-1])
            self.assertTrue(replay_result["ok"])
            self.assertEqual(capture["processing"]["samples"], replay_result["processing"]["samples"])
            self.assertEqual(capture["processing"]["factor_crc32"], replay_result["processing"]["factor_crc32"])
            self.assertEqual(capture["processing"]["last_ingress_sequence"], replay_result["processing"]["last_ingress_sequence"])
            self.assertEqual(bound["environment"]["execution"], "disabled")
            return capture, replay_result

    def test_sz_factors_capture_and_replay(self):
        self.capture_replay("SZ", sze_packets())

    def test_sh_factors_capture_and_replay(self):
        self.capture_replay("SH", sse_packets(), idle_gap=100000)

    def test_wrong_market_rejects_before_capture_directory_creation(self):
        with tempfile.TemporaryDirectory(prefix="market-cli-") as directory:
            recording = os.path.join(directory, "recording")
            profile, _ = self.bound_profile(recording, "SH")
            stream = os.path.join(directory, "stream.json")
            write_json(stream, self.stream_config(recording, port()))
            result = self.run_cli(self.sze_binary, "capture", stream, profile)
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(os.path.exists(recording))

    def test_old_sse_global_contract_rejects_before_capture(self):
        with tempfile.TemporaryDirectory(prefix="market-cli-") as directory:
            recording = os.path.join(directory, "recording")
            profile, _ = self.bound_profile(recording, "SH")
            with open(profile) as source:
                value = json.load(source)
            value["processing_contract"] = "sse-batch-end-v1"
            write_json(profile, value)
            stream = os.path.join(directory, "stream.json")
            write_json(stream, self.stream_config(recording, port(), 100000))
            result = self.run_cli(self.sse_binary, "capture", stream, profile)
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(os.path.exists(recording))

    def test_live_execution_profile_rejects_before_socket(self):
        with tempfile.TemporaryDirectory(prefix="market-cli-") as directory:
            recording = os.path.join(directory, "recording")
            profile, _ = self.bound_profile(recording, "SZ")
            with open(profile) as source:
                value = json.load(source)
            value["execution"] = "live"
            write_json(profile, value)
            stream = os.path.join(directory, "stream.json")
            write_json(stream, self.stream_config(recording, port()))
            result = self.run_cli(self.sze_binary, "capture", stream, profile)
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(os.path.exists(recording))

    def test_environment_driver_mismatch_rejects_before_capture(self):
        with tempfile.TemporaryDirectory(prefix="market-cli-") as directory:
            recording = os.path.join(directory, "recording")
            profile, _ = self.bound_profile(recording, "SZ", mode="replay")
            stream = os.path.join(directory, "stream.json")
            write_json(stream, self.stream_config(recording, port()))
            result = self.run_cli(self.sze_binary, "capture", stream, profile)
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(os.path.exists(recording))

    def test_missing_prediction_model_fails_before_socket(self):
        with tempfile.TemporaryDirectory(prefix="market-cli-") as directory:
            recording = os.path.join(directory, "recording")
            profile, _ = self.bound_profile(recording, "SZ", factors_only=False)
            stream = os.path.join(directory, "stream.json")
            write_json(stream, self.stream_config(recording, port()))
            result = self.run_cli(self.sze_binary, "capture", stream, profile)
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(os.path.exists(recording))

    def test_replay_changed_recording_path_rejects(self):
        with tempfile.TemporaryDirectory(prefix="market-cli-") as directory:
            expected = os.path.join(directory, "expected")
            changed = os.path.join(directory, "changed")
            os.mkdir(expected)
            os.mkdir(changed)
            profile, _ = self.bound_profile(expected, "SZ", mode="replay")
            result = self.run_cli(self.sze_binary, "replay", changed, profile)
            self.assertNotEqual(0, result.returncode)

    def test_duplicate_profile_keys_reject(self):
        with tempfile.TemporaryDirectory(prefix="market-cli-") as directory:
            recording = os.path.join(directory, "recording")
            stream = os.path.join(directory, "stream.json")
            write_json(stream, self.stream_config(recording, port()))
            profile = os.path.join(directory, "duplicate.json")
            with open(profile, "w") as output:
                output.write('{"schema_version":1,"schema_version":1}')
            result = self.run_cli(self.sze_binary, "capture", stream, profile)
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(os.path.exists(recording))

    def test_sse_wrong_idle_gap_rejects_before_socket(self):
        with tempfile.TemporaryDirectory(prefix="market-cli-") as directory:
            recording = os.path.join(directory, "recording")
            profile, _ = self.bound_profile(recording, "SH")
            stream = os.path.join(directory, "stream.json")
            write_json(stream, self.stream_config(recording, port(), idle_gap=0))
            result = self.run_cli(self.sse_binary, "capture", stream, profile)
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(os.path.exists(recording))


if __name__ == "__main__":
    CLI_ARGS = tuple(sys.argv[1:])
    sys.argv = sys.argv[:1]
    unittest.main()
