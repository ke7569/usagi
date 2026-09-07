#!/usr/bin/env python3
"""Subprocess-isolated C ABI checks using loopback and factors-only profiles."""

import ctypes
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import unittest

import test_market_processing_cli as market_cli


CLI_ARGS = ()
CharBuffer = ctypes.POINTER(ctypes.c_char)
Create = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                         ctypes.c_char_p, CharBuffer, ctypes.c_size_t)
Start = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, CharBuffer, ctypes.c_size_t)
RequestStop = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
Join = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, CharBuffer, ctypes.c_size_t)
StatusJson = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p, CharBuffer, ctypes.c_size_t)
Destroy = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class RuntimeApiV1(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_bytes", ctypes.c_uint32),
        ("market", ctypes.c_char_p),
        ("create", Create),
        ("start", Start),
        ("request_stop", RequestStop),
        ("join", Join),
        ("status_json", StatusJson),
        ("destroy", Destroy),
    ]


def encoded(value):
    return None if value is None else os.fsencode(value)


class LoadedRuntime:
    def __init__(self, path):
        self.library = ctypes.CDLL(path, mode=ctypes.RTLD_LOCAL)
        getter = self.library.t0_market_runtime_v1
        getter.argtypes = []
        getter.restype = ctypes.POINTER(RuntimeApiV1)
        self.pointer = getter()
        if not self.pointer:
            raise AssertionError("null runtime API")
        self.api = self.pointer.contents
        if self.api.abi_version != 1 or self.api.struct_bytes < ctypes.sizeof(RuntimeApiV1):
            raise AssertionError("runtime API layout/version mismatch")

    def create(self, action, stream_or_recording, profile):
        error = ctypes.create_string_buffer(4096)
        handle = self.api.create(encoded(action), encoded(stream_or_recording), encoded(profile),
                                 error, len(error))
        return handle, error.value.decode("utf-8", "replace")

    def start(self, handle):
        error = ctypes.create_string_buffer(4096)
        result = self.api.start(handle, error, len(error))
        return result, error.value.decode("utf-8", "replace")

    def join(self, handle):
        error = ctypes.create_string_buffer(4096)
        result = self.api.join(handle, error, len(error))
        return result, error.value.decode("utf-8", "replace")

    def status(self, handle):
        # Status may change between sizing and copying while a worker runs.
        for _ in range(20):
            needed = self.api.status_json(handle, None, 0)
            if not 1 < needed < 1024 * 1024:
                raise AssertionError("invalid status JSON size: {}".format(needed))
            output = ctypes.create_string_buffer(needed)
            actual = self.api.status_json(handle, output, len(output))
            if actual > len(output):
                if output.value:
                    raise AssertionError("small status buffer was not cleared")
                continue
            if actual != len(output.value) + 1:
                raise AssertionError("status size does not include exactly one NUL")
            return json.loads(output.value.decode("utf-8"))
        raise AssertionError("status changed size too often")


class ApiScenario(unittest.TestCase):
    """Each case runs in a disposable child process, including native calls."""

    def __init__(self, args):
        super().__init__("runTest")
        self.runtimes = {"SH": LoadedRuntime(args[0]), "SZ": LoadedRuntime(args[1])}
        self.binaries = {"SH": args[2], "SZ": args[3]}
        self.fixture = market_cli.MarketProcessingCliTests()
        self.serial = 0

    def inputs(self, market, duration=200, network_override=None):
        self.serial += 1
        directory = os.path.join(self.directory, "{}-{}".format(market, self.serial))
        os.mkdir(directory)
        recording = os.path.join(directory, "recording")
        profile, bound = self.fixture.bound_profile(recording, market, factors_only=True)
        receive_port = market_cli.port()
        network = self.fixture.stream_config(recording, receive_port,
                                             100000 if market == "SH" else 0)
        network["duration_ms"] = duration
        if network_override:
            network.update(network_override)
        stream = os.path.join(directory, "stream.json")
        market_cli.write_json(stream, network)
        return stream, profile, recording, receive_port, bound

    def checked_create(self, runtime, action, stream_or_recording, profile):
        handle, error = runtime.create(action, stream_or_recording, profile)
        self.assertTrue(handle, error)
        self.assertEqual("", error)
        return handle

    def check_status(self, status):
        self.assertIsInstance(status["state"], str)
        self.assertIs(type(status["ready"]), bool)
        self.assertIs(type(status["done"]), bool)

    def wait_ready(self, runtime, handle):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            status = runtime.status(handle)
            self.check_status(status)
            if status["ready"]:
                return status
            self.assertFalse(status["done"], status)
            time.sleep(0.001)
        self.fail("runtime did not report readiness before deadline")

    def case_load_both(self):
        for market in ("SH", "SZ"):
            runtime = self.runtimes[market]
            self.assertEqual(market, runtime.api.market.decode("ascii"))
            stream, profile, recording, _, _ = self.inputs(market)
            handle = self.checked_create(runtime, "capture", stream, profile)
            try:
                status = runtime.status(handle)
                self.check_status(status)
                self.assertFalse(status["ready"])
                self.assertFalse(status["done"])
                self.assertFalse(os.path.exists(recording))
            finally:
                runtime.api.destroy(handle)

    def case_invalid_create(self):
        for market, runtime in self.runtimes.items():
            stream, profile, recording, _, _ = self.inputs(market)
            malformed = os.path.join(self.directory, "bad-{}.json".format(market))
            with open(malformed, "w") as output:
                output.write('{"schema_version":1,"schema_version":1}')
            for action, network, config in (
                    (None, None, None), ("", stream, profile),
                    ("unsupported", stream, profile), ("capture", "", profile),
                    ("capture", os.path.join(self.directory, "missing.json"), profile),
                    ("capture", stream, ""), ("capture", stream, malformed)):
                handle, error = runtime.create(action, network, config)
                if handle:
                    runtime.api.destroy(handle)
                self.assertFalse(handle, (market, action, network, config))
                self.assertTrue(error)
                self.assertFalse(os.path.exists(recording))

    def capture_replay(self, market):
        runtime = self.runtimes[market]
        stream, profile, recording, receive_port, bound = self.inputs(market)
        handle = self.checked_create(runtime, "capture", stream, profile)
        packets = market_cli.sse_packets() if market == "SH" else market_cli.sze_packets()[:24]
        try:
            result, error = runtime.start(handle)
            self.assertEqual(1, result, error)
            self.wait_ready(runtime, handle)
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                for packet in packets:
                    sender.sendto(packet, ("127.0.0.1", receive_port))
                    time.sleep(0.002)
            finally:
                sender.close()
            result, error = runtime.join(handle)
            self.assertEqual(1, result, error)
            captured = runtime.status(handle)
            self.check_status(captured)
            self.assertTrue(captured["done"])
            self.assertTrue(captured["ok"])
            self.assertEqual(len(packets), captured["datagrams"])
            self.assertGreater(captured["processing"]["samples"], 0)
            self.assertEqual(0, captured["processing"]["predictions"])
            self.assertEqual("disabled", captured["processing"]["execution"])
            self.assertTrue(captured["stats"]["clean_recording"])
        finally:
            runtime.api.destroy(handle)

        replay_bound = market_cli.unified_config.bind_environment(
            bound, "replay", recording, "t0md-v1", "recorded-receive")
        replay_profile = os.path.join(os.path.dirname(recording), "replay.json")
        market_cli.write_json(replay_profile, market_cli.prepare.make_profile(replay_bound, factors_only=True))
        cli = subprocess.run([self.binaries[market], "replay", recording, replay_profile],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=8)
        self.assertEqual(0, cli.returncode, cli.stderr.decode("utf-8", "replace"))
        cli_result = json.loads(cli.stdout.decode().splitlines()[-1])
        self.assertTrue(cli_result["ok"])

        handle = self.checked_create(runtime, "replay", recording, replay_profile)
        try:
            result, error = runtime.start(handle)
            self.assertEqual(1, result, error)
            result, error = runtime.join(handle)
            self.assertEqual(1, result, error)
            replayed = runtime.status(handle)
            self.check_status(replayed)
            self.assertTrue(replayed["done"])
            self.assertTrue(replayed["ok"])
            for result in (cli_result, replayed):
                self.assertEqual(captured["processing"], result["processing"])
                for key in ("datagrams", "idle_events", "payload_bytes"):
                    self.assertEqual(captured[key], result[key], key)
                for key in ("received_datagrams", "receive_batches"):
                    self.assertEqual(captured["stats"][key], result["stats"][key], key)
        finally:
            runtime.api.destroy(handle)

    def case_capture_replay_sh(self):
        self.capture_replay("SH")

    def case_capture_replay_sz(self):
        self.capture_replay("SZ")

    def case_stop_before_start(self):
        for market, runtime in self.runtimes.items():
            stream, profile, recording, _, _ = self.inputs(market)
            handle = self.checked_create(runtime, "capture", stream, profile)
            try:
                runtime.api.request_stop(handle)
                runtime.api.request_stop(handle)
                result, error = runtime.start(handle)
                self.assertEqual(0, result)
                self.assertTrue(error)
                self.assertFalse(os.path.exists(recording))
            finally:
                runtime.api.destroy(handle)

    def case_repeated_calls_and_status_buffers(self):
        for market, runtime in self.runtimes.items():
            stream, profile, _, _, _ = self.inputs(market)
            handle = self.checked_create(runtime, "capture", stream, profile)
            try:
                needed = runtime.api.status_json(handle, None, 0)
                self.assertGreater(needed, 1)
                tiny = ctypes.create_string_buffer(b"x", 2)
                self.assertEqual(needed, runtime.api.status_json(handle, tiny, len(tiny)))
                self.assertEqual(b"", tiny.value)
                exact = ctypes.create_string_buffer(needed)
                self.assertEqual(needed, runtime.api.status_json(handle, exact, len(exact)))
                self.assertEqual(needed, len(exact.value) + 1)
                self.check_status(json.loads(exact.value.decode()))
                result, error = runtime.start(handle)
                self.assertEqual(1, result, error)
                result, error = runtime.start(handle)
                self.assertEqual(0, result)
                self.assertTrue(error)
                self.wait_ready(runtime, handle)
                runtime.api.request_stop(handle)
                runtime.api.request_stop(handle)
                first = runtime.join(handle)
                second = runtime.join(handle)
                self.assertEqual((1, ""), first)
                self.assertEqual(first, second)
                self.assertTrue(runtime.status(handle)["done"])
                result, error = runtime.start(handle)
                self.assertEqual(0, result)
                self.assertTrue(error)
            finally:
                runtime.api.destroy(handle)

    def case_immediate_stop_and_destroy(self):
        for market, runtime in self.runtimes.items():
            for iteration in range(6):
                stream, profile, _, _, _ = self.inputs(market, duration=0)
                handle = self.checked_create(runtime, "capture", stream, profile)
                result, error = runtime.start(handle)
                self.assertEqual(1, result, error)
                # No readiness delay: exercise the stop/start worker handoff.
                if iteration % 2 == 0:
                    runtime.api.request_stop(handle)
                    runtime.api.request_stop(handle)
                before = time.monotonic()
                runtime.api.destroy(handle)
                self.assertLess(time.monotonic() - before, 4)

    def case_runtime_network_failure(self):
        for market, runtime in self.runtimes.items():
            stream, profile, _, _, _ = self.inputs(market)
            network = market_cli.unified_config.load_json(stream)
            network["channels"][0]["group"] = "not-an-ip"
            market_cli.write_json(stream, network)
            handle = self.checked_create(runtime, "capture", stream, profile)
            try:
                result, error = runtime.start(handle)
                self.assertEqual(1, result, error)
                result, error = runtime.join(handle)
                self.assertEqual(0, result)
                self.assertTrue(error)
                status = runtime.status(handle)
                self.check_status(status)
                self.assertTrue(status["done"])
                self.assertFalse(status["ok"])
                self.assertTrue(status["error"])
                repeated, repeated_error = runtime.join(handle)
                self.assertEqual((result, error), (repeated, repeated_error))
            finally:
                runtime.api.destroy(handle)

    def case_status_while_join_waits(self):
        for market, runtime in self.runtimes.items():
            stream, profile, _, _, _ = self.inputs(market, duration=0)
            handle = self.checked_create(runtime, "capture", stream, profile)
            outcome = []
            worker = None
            try:
                result, error = runtime.start(handle)
                self.assertEqual(1, result, error)
                self.wait_ready(runtime, handle)
                worker = threading.Thread(target=lambda: outcome.append(runtime.join(handle)))
                worker.daemon = True
                worker.start()
                for _ in range(20):
                    status = runtime.status(handle)
                    self.check_status(status)
                    self.assertFalse(status["done"])
                runtime.api.request_stop(handle)
                worker.join(timeout=4)
                self.assertFalse(worker.is_alive(), "join did not observe request_stop")
                self.assertEqual([(1, "")], outcome)
                self.assertTrue(runtime.status(handle)["done"])
            finally:
                runtime.api.request_stop(handle)
                if worker is not None:
                    worker.join(timeout=4)
                    self.assertFalse(worker.is_alive(), "join still running before destroy")
                runtime.api.destroy(handle)

    def thread_creation_failure(self, market):
        runtime = self.runtimes[market]
        stream, profile, recording, _, _ = self.inputs(market)
        handle = self.checked_create(runtime, "capture", stream, profile)
        try:
            result, start_error = runtime.start(handle)
            self.assertEqual(0, result)
            self.assertTrue(start_error)
            status = runtime.status(handle)
            self.check_status(status)
            self.assertTrue(status["done"])
            self.assertEqual("failed", status["state"])
            self.assertFalse(status["ready"])
            self.assertFalse(status["ok"])
            self.assertEqual(start_error, status["error"])
            self.assertFalse(os.path.exists(recording))
            for _ in range(2):
                joined, join_error = runtime.join(handle)
                self.assertEqual(0, joined)
                self.assertEqual(start_error, join_error)
            runtime.api.request_stop(handle)
        finally:
            runtime.api.destroy(handle)
        self.assertFalse(os.path.exists(recording))

    def case_thread_creation_failure_sh(self):
        self.thread_creation_failure("SH")

    def case_thread_creation_failure_sz(self):
        self.thread_creation_failure("SZ")


class MarketRuntimeApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(CLI_ARGS) not in (4, 5):
            raise unittest.SkipTest("usage: test_market_runtime_api.py SSE_LIB SZE_LIB SSE_BINARY SZE_BINARY [SHIM_LIB]")

    def isolated(self, case, fail_thread=None):
        environment = os.environ.copy()
        environment.pop("T0_TEST_FAIL_PTHREAD_CREATE_AT", None)
        if fail_thread is not None:
            if len(CLI_ARGS) != 5:
                self.skipTest("thread creation fault test requires SHIM_LIB")
            shim = os.path.abspath(CLI_ARGS[4])
            self.assertTrue(os.path.isfile(shim), shim)
            environment["LD_PRELOAD"] = shim + (
                ":" + environment["LD_PRELOAD"] if environment.get("LD_PRELOAD") else "")
            environment["T0_TEST_FAIL_PTHREAD_CREATE_AT"] = str(fail_thread)
        result = subprocess.run([sys.executable, "-B", os.path.abspath(__file__),
                                 "--worker", case] + list(CLI_ARGS[:4]),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20,
                                env=environment)
        self.assertEqual(0, result.returncode,
                         (result.stdout + result.stderr).decode("utf-8", "replace"))

    def test_api_market_and_layout_isolation(self):
        self.isolated("case_load_both")

    def test_empty_and_invalid_create_configs(self):
        self.isolated("case_invalid_create")

    def test_sse_capture_cli_and_api_replay(self):
        self.isolated("case_capture_replay_sh")

    def test_sze_capture_cli_and_api_replay(self):
        self.isolated("case_capture_replay_sz")

    def test_stop_before_start_creates_no_recording(self):
        self.isolated("case_stop_before_start")

    def test_single_start_repeated_stop_join_and_small_status_buffer(self):
        self.isolated("case_repeated_calls_and_status_buffers")

    def test_immediate_stop_destroy_repeated(self):
        self.isolated("case_immediate_stop_and_destroy")

    def test_runtime_network_failure_reaches_status_and_join(self):
        self.isolated("case_runtime_network_failure")

    def test_concurrent_status_and_stop_while_join_waits(self):
        self.isolated("case_status_while_join_waits")

    def test_first_thread_creation_failure_preserves_error_and_reclaims_handle(self):
        for market in ("sh", "sz"):
            with self.subTest(market=market):
                self.isolated("case_thread_creation_failure_" + market, fail_thread=1)

    def test_second_thread_creation_failure_joins_stop_worker(self):
        for market in ("sh", "sz"):
            with self.subTest(market=market):
                self.isolated("case_thread_creation_failure_" + market, fail_thread=2)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--worker":
        try:
            scenario = ApiScenario(tuple(sys.argv[3:]))
            with tempfile.TemporaryDirectory(prefix="market-runtime-api-") as directory:
                scenario.directory = directory
                getattr(scenario, sys.argv[2])()
            print(json.dumps({"case": sys.argv[2], "ok": True}))
        except Exception:
            traceback.print_exc()
            sys.exit(1)
    else:
        CLI_ARGS = tuple(sys.argv[1:])
        sys.argv = sys.argv[:1]
        unittest.main()
