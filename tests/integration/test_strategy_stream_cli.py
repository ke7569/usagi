#!/usr/bin/env python3
"""Loopback ZStrategy intent checks using temporary synthetic model assets."""

import copy
import json
import os
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest

import test_market_processing_cli as market_cli


prepare = market_cli.prepare
unified_config = market_cli.unified_config
CLI_ARGS = ()


def tick(index, kind, side, exchange_time, buy_id, sell_id,
         price=10000, quantity=100000):
    packet = bytearray(market_cli.sse_tick(
        index, index, kind, side, exchange_time, buy_id, sell_id, quantity))
    market_cli.put_u32(packet, 51, price)
    return bytes(packet)


def strategy_packets():
    packets = []
    index = 1
    for level in range(10):
        packets.append(tick(index, "A", 0, 9410000, 1001 + level, 0,
                            price=9990 - 10 * level, quantity=10000000))
        index += 1
        packets.append(tick(index, "A", 1, 9410000, 0, 2001 + level,
                            price=10010 + 10 * level, quantity=10000000))
        index += 1
    for second in range(1, 7):
        packets.append(tick(index, "T", 0, 9410000 + second * 100,
                            1001, 2001))
        index += 1
    # A later observed event drives already-due cancels without inventing EOF time.
    delayed_observation = tick(index, "A", 0, 9410800, 1099, 0,
                               price=9880, quantity=10000000)
    return packets, delayed_observation


class StrategyStreamCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(CLI_ARGS) != 2:
            raise unittest.SkipTest("usage: test_strategy_stream_cli.py SSE_BINARY FIXTURE_BINARY")
        cls.binary, cls.fixture_binary = CLI_ARGS

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="strategy-stream-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = self.temporary.name
        self.assets = os.path.join(self.directory, "synthetic-models")
        os.mkdir(self.assets)
        generated = subprocess.run([self.fixture_binary, self.assets], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(0, generated.returncode, generated.stderr.decode("utf-8", "replace"))

    def runtime(self):
        return {
            "market": "SH", "trading_day": 20260904,
            "strategy_name": "synthetic-strategy-stream",
            "model_type": "sse_hybrid_native",
            "sh_orderbook_mode": "complete-orderbook-sh",
            "sze_startup_warmup_signals": 0,
            "td_source_index": [],
            "oms": {"simulation_cash": 1000000, "fee_reserve_per_order": 0},
            "model_path": os.path.join(self.assets, "tick.bin"),
            "snapshot_baseline_model_path": os.path.join(self.assets, "baseline.ssegru"),
            "snapshot_baseline_scaler_path": os.path.join(self.assets, "baseline.json"),
            "snapshot_auction59_model_path": os.path.join(self.assets, "auction59.ssegru"),
            "snapshot_auction59_scaler_path": os.path.join(self.assets, "auction59.json"),
            "global_params": {"offset": 0.25, "global_bias_factor": 1.0,
                              "position_limit": 1000.0, "position_base_line": 100000.0},
            "ins_params": {"600000.SH": {
                "Date": 20260904, "Close": 10.0, "HistoryAmount": 8000000.0,
                "FreeShare": 10000000.0, "HpUpperPrice": 11.0, "HpLowerPrice": 9.0,
                "HistoryVolatility20d": 0.02, "static_position": 1000,
                "last_position": 0, "min_order_size": 100, "vol_unit": 100,
            }},
        }

    def live_profile(self):
        config = unified_config.migrate_runtime(self.runtime(), "PAPER-TEST")
        bound = unified_config.bind_environment(config, "live")
        return prepare.make_profile(bound, strategy_intents=True), bound

    def stream_config(self, recording, receive_port):
        return {
            "channels": [{"name": "loopback", "group": "127.0.0.1",
                          "interface_ip": "127.0.0.1", "port": receive_port}],
            "recording_directory": recording, "recording_required": True,
            "duration_ms": 1600, "queue_capacity": 4096, "max_datagram_bytes": 1024,
            "receive_batch_size": 64, "receive_buffer_bytes": 1048576,
            "idle_gap_ns": 100000, "segment_bytes": 32768, "flush_interval_ms": 5,
            "receive_cpu": -1, "dispatch_cpu": -1, "writer_cpu": -1,
        }

    def invoke(self, action, stream_or_recording, profile):
        return subprocess.run([self.binary, action, stream_or_recording, profile],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=8)

    def assert_rejects_before_capture(self, profile):
        recording = os.path.join(self.directory, "must-not-be-created")
        stream_path = os.path.join(self.directory, "invalid-stream.json")
        profile_path = os.path.join(self.directory, "invalid-profile.json")
        market_cli.write_json(stream_path, self.stream_config(recording, market_cli.port()))
        market_cli.write_json(profile_path, profile)
        result = self.invoke("capture", stream_path, profile_path)
        self.assertNotEqual(0, result.returncode, result.stdout.decode("utf-8", "replace"))
        self.assertFalse(os.path.exists(recording))
        self.assertNotIn(b"market-data stream ready", result.stderr)

    def test_strategy_orders_and_due_cancels_match_capture_and_replay(self):
        profile, bound = self.live_profile()
        profile_path = os.path.join(self.directory, "live-profile.json")
        stream_path = os.path.join(self.directory, "stream.json")
        recording = os.path.join(self.directory, "recording")
        receive_port = market_cli.port()
        market_cli.write_json(profile_path, profile)
        market_cli.write_json(stream_path, self.stream_config(recording, receive_port))
        process = subprocess.Popen([self.binary, "capture", stream_path, profile_path],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        startup = []
        try:
            ready = False
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and process.poll() is None:
                readable, _, _ = select.select([process.stderr], [], [], 0.1)
                if readable:
                    line = process.stderr.readline()
                    startup.append(line)
                    if b"market-data stream ready" in line:
                        ready = True
                        break
            self.assertTrue(ready, b"".join(startup).decode("utf-8", "replace"))
            packets, delayed_observation = strategy_packets()
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                for packet in packets:
                    sender.sendto(packet, ("127.0.0.1", receive_port))
                    time.sleep(0.002)
                # The transport emits only observed idle events, not periodic timers.
                time.sleep(1.10)
                sender.sendto(delayed_observation, ("127.0.0.1", receive_port))
            finally:
                sender.close()
            stdout, stderr = process.communicate(timeout=8)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        self.assertEqual(0, process.returncode, stderr.decode("utf-8", "replace"))
        capture = json.loads(stdout.decode().splitlines()[-1])
        self.assertTrue(capture["ok"])
        self.assertTrue(capture["stats"]["clean_recording"])
        self.assertEqual(len(packets) + 1, capture["datagrams"])
        processing = capture["processing"]
        strategy = processing["strategy"]
        self.assertGreaterEqual(processing["samples"], 3)
        self.assertGreaterEqual(strategy["signals"], 3)
        self.assertGreater(strategy["order_intents"], 0, strategy)
        self.assertGreater(strategy["cancel_intents"], 0)
        self.assertFalse(strategy["fills_simulated"])
        self.assertEqual("paper-intents", strategy["mode"])
        self.assertEqual("disabled", processing["execution"])

        replay_bound = unified_config.bind_environment(
            bound, "replay", recording, "t0md-v1", "recorded-receive")
        replay_profile = prepare.make_profile(replay_bound, strategy_intents=True)
        replay_path = os.path.join(self.directory, "replay-profile.json")
        market_cli.write_json(replay_path, replay_profile)
        replay = self.invoke("replay", recording, replay_path)
        self.assertEqual(0, replay.returncode, replay.stderr.decode("utf-8", "replace"))
        result = json.loads(replay.stdout.decode().splitlines()[-1])
        self.assertTrue(result["ok"])
        for key in ("signals", "order_intents", "cancel_intents", "intent_crc32"):
            self.assertEqual(strategy[key], result["processing"]["strategy"][key], key)
        for key in ("factor_crc32", "samples", "predictions", "processing_sha256"):
            self.assertEqual(processing[key], result["processing"][key], key)

    def test_live_execution_is_rejected_before_capture(self):
        profile, _ = self.live_profile()
        for placement in ("top-level", "environment"):
            changed = copy.deepcopy(profile)
            if placement == "top-level":
                changed["execution"] = "live"
            else:
                changed["environment"]["execution"] = "live"
            with self.subTest(placement=placement):
                self.assert_rejects_before_capture(changed)

    def test_factors_only_strategy_is_rejected_before_capture(self):
        profile, _ = self.live_profile()
        profile["processing_mode"] = "factors-only"
        self.assert_rejects_before_capture(profile)

    def test_mismatched_strategy_universe_is_rejected_before_capture(self):
        profile, _ = self.live_profile()
        inputs = profile["strategy_runtime"]["legacy_config"]["ins_params"]
        inputs["600001.SH"] = inputs.pop("600000.SH")
        self.assert_rejects_before_capture(profile)

    def test_invalid_execution_sources_are_rejected_before_capture(self):
        profile, _ = self.live_profile()
        for sources in ({}, "", [0], [True], [32768], [1, 2]):
            changed = copy.deepcopy(profile)
            changed["strategy_runtime"]["legacy_config"]["td_source_index"] = sources
            with self.subTest(sources=sources):
                self.assert_rejects_before_capture(changed)


if __name__ == "__main__":
    CLI_ARGS = tuple(sys.argv[1:])
    sys.argv = sys.argv[:1]
    unittest.main()
