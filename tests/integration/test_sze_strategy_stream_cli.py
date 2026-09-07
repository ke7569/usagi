#!/usr/bin/env python3
"""Offline SZE raw-stream -> model -> StrategySession paper-intent coverage."""

import importlib.util
import json
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
TOOLS = os.path.join(ROOT, "tools", "config")
sys.path.insert(0, TOOLS)
import prepare_stream_processing as prepare
import unified_config


def load_market_helpers():
    path = os.path.join(os.path.dirname(__file__), "test_market_processing_cli.py")
    spec = importlib.util.spec_from_file_location("market_processing_helpers", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HELPERS = load_market_helpers()


def write_json(path, value):
    with open(path, "w") as stream:
        json.dump(value, stream, sort_keys=True)


def port():
    handle = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    handle.bind(("127.0.0.1", 0))
    value = handle.getsockname()[1]
    handle.close()
    return value


def runtime(model_path, static_position=100000):
    return {
        "market": "SZ",
        "trading_day": 20260904,
        "model_path": model_path,
        "global_params": {
            "offset": 1.0,
            "global_bias_factor": 1.0,
            "position_base_line": 100000.0,
            "position_limit": 1000.0,
        },
        "sze_startup_warmup_signals": 0,
        "oms": {"simulation_cash": 1000000, "fee_reserve_per_order": 0},
        "ins_params": {
            "000001.SZ": {
                "Date": 20260904,
                "Close": 10.0,
                "HistoryAmount": 8000000.0,
                "FreeShare": 10000000.0,
                "HpUpperPrice": 11.0,
                "HpLowerPrice": 9.0,
                "HistoryVolatility20d": 0.02,
                "static_position": static_position,
                "last_position": 0,
            }
        },
    }


def stream_config(recording, stream_port):
    return {
        "channels": [{"name": "loopback", "group": "127.0.0.1",
                      "interface_ip": "127.0.0.1", "port": stream_port}],
        "recording_directory": recording,
        "recording_required": True,
        "duration_ms": 1500,
        "queue_capacity": 4096,
        "max_datagram_bytes": 1024,
        "receive_batch_size": 64,
        "receive_buffer_bytes": 1048576,
        "idle_gap_ns": 0,
        "segment_bytes": 32768,
        "flush_interval_ms": 5,
        "receive_cpu": -1,
        "dispatch_cpu": -1,
        "writer_cpu": -1,
    }


class SzeStrategyStreamCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(CLI_ARGS) != 2:
            raise unittest.SkipTest("usage: test_sze_strategy_stream_cli.py SZE_BINARY FIXTURE_BINARY")
        cls.sze_binary, cls.fixture_binary = CLI_ARGS

    def invoke_fixture(self, path):
        result = subprocess.run([self.fixture_binary, path],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(result.returncode, 0,
                         result.stderr.decode("utf-8", "replace"))
        overwrite = subprocess.run([self.fixture_binary, path],
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=5)
        self.assertNotEqual(overwrite.returncode, 0)

    def make_profile(self, directory, model_path, replay=False):
        migrated = unified_config.migrate_runtime(
            runtime(model_path), "SZE-STRATEGY-CLI")
        if replay:
            bound = unified_config.bind_environment(
                migrated, "replay", directory, "t0md-v1", "recorded-receive")
        else:
            bound = unified_config.bind_environment(migrated, "live")
        profile = prepare.make_profile(bound, strategy_intents=True)
        path = os.path.join(os.path.dirname(directory),
                            "profile-replay.json" if replay else "profile-live.json")
        write_json(path, profile)
        return path

    def run_capture(self, recording, profile, packets):
        stream_path = os.path.join(os.path.dirname(recording), "stream.json")
        stream_port = port()
        write_json(stream_path, stream_config(recording, stream_port))
        process = subprocess.Popen(
            [self.sze_binary, "capture", stream_path, profile],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        ready = False
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and process.poll() is None:
            readable, _, _ = select.select([process.stderr], [], [], 0.1)
            if readable:
                line = process.stderr.readline()
                if b"market-data stream ready" in line:
                    ready = True
                    break
        self.assertTrue(ready, "SZE stream did not become ready")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            for packet in packets:
                sender.sendto(packet, ("127.0.0.1", stream_port))
        stdout, stderr = process.communicate(timeout=8)
        self.assertEqual(process.returncode, 0,
                         stderr.decode("utf-8", "replace"))
        return json.loads(stdout.decode("utf-8").splitlines()[-1])

    def run_replay(self, recording, profile):
        result = subprocess.run(
            [self.sze_binary, "replay", recording, profile],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=8)
        self.assertEqual(result.returncode, 0,
                         result.stderr.decode("utf-8", "replace"))
        return json.loads(result.stdout.decode("utf-8").splitlines()[-1])

    def test_capture_replay_strategy_intents_and_model(self):
        with tempfile.TemporaryDirectory(prefix="sze-strategy-cli-") as directory:
            model = os.path.join(directory, "synthetic.mix153060")
            self.invoke_fixture(model)
            recording = os.path.join(directory, "recording")
            live_profile = self.make_profile(recording, model)
            packets = [
                HELPERS.sze_order(1, 1, True, 0),
                HELPERS.sze_order(2, 2, False, 0),
            ]
            for packet in HELPERS.sze_packets():
                shifted = bytearray(packet)
                sequence = struct.unpack_from("<I", shifted, 0)[0]
                struct.pack_into("<I", shifted, 0, sequence + 2)
                application = struct.unpack_from("<Q", shifted, 31)[0]
                struct.pack_into("<Q", shifted, 31, application + 2)
                if shifted[8] == 24:
                    buy_id = struct.unpack_from("<q", shifted, 43)[0]
                    sell_id = struct.unpack_from("<q", shifted, 51)[0]
                    struct.pack_into("<q", shifted, 43, buy_id + 2)
                    struct.pack_into("<q", shifted, 51, sell_id + 2)
                packets.append(bytes(shifted))
            capture = self.run_capture(recording, live_profile, packets)
            self.assertTrue(capture["ok"])
            self.assertEqual(capture["health"]["permits_new_risk"], True)
            self.assertGreater(capture["processing"]["samples"], 0)
            self.assertGreater(capture["processing"]["predictions"], 0)
            self.assertGreater(capture["processing"]["strategy"]["signals"], 0)
            self.assertGreater(capture["processing"]["strategy"]["order_intents"], 0)
            self.assertEqual(capture["processing"]["execution"], "disabled")

            replay_profile = self.make_profile(recording, model, replay=True)
            replay = self.run_replay(recording, replay_profile)
            self.assertTrue(replay["ok"])
            self.assertEqual(capture["processing"]["samples"],
                             replay["processing"]["samples"])
            self.assertEqual(capture["processing"]["predictions"],
                             replay["processing"]["predictions"])
            self.assertEqual(capture["processing"]["factor_crc32"],
                             replay["processing"]["factor_crc32"])
            self.assertEqual(capture["processing"]["strategy"]["signals"],
                             replay["processing"]["strategy"]["signals"])
            self.assertEqual(capture["processing"]["strategy"]["order_intents"],
                             replay["processing"]["strategy"]["order_intents"])
            self.assertEqual(capture["processing"]["strategy"]["intent_crc32"],
                             replay["processing"]["strategy"]["intent_crc32"])

    def test_account_reference_and_baseline_required_before_capture(self):
        with tempfile.TemporaryDirectory(prefix="sze-strategy-config-") as directory:
            model = os.path.join(directory, "synthetic.mix153060")
            self.invoke_fixture(model)
            for missing_reference, incomplete_baseline in ((True, False), (False, True)):
                recording = os.path.join(directory, "reject-%d-%d" %
                                          (missing_reference, incomplete_baseline))
                profile_path = self.make_profile(recording, model)
                with open(profile_path) as stream:
                    profile = json.load(stream)
                if missing_reference:
                    profile["strategy_runtime"].pop("account_reference", None)
                if incomplete_baseline:
                    profile["strategy_runtime"]["legacy_config"]["ins_params"][
                        "000001.SZ"].pop("last_position", None)
                write_json(profile_path, profile)
                stream_path = os.path.join(directory, "reject-stream.json")
                write_json(stream_path, stream_config(recording, port()))
                result = subprocess.run(
                    [self.sze_binary, "capture", stream_path, profile_path],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(os.path.exists(recording))


if __name__ == "__main__":
    CLI_ARGS = tuple(sys.argv[1:])
    sys.argv = sys.argv[:1]
    unittest.main()
