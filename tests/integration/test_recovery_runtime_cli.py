import argparse
import ctypes
import copy
import json
import os
import select
import signal
import subprocess
import tempfile
import time
import unittest

_TOOLS = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../tools/config"))
_SRC_TOOLS = _TOOLS
import sys
sys.path.insert(0, _SRC_TOOLS)
import prepare_stream_processing as prepare
import unified_config


class _Api(ctypes.Structure):
    pass


_Create = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                           ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t)
_Start = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t)
_Stop = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
_Join = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t)
_Status = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t)
_Destroy = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
_Api._fields_ = [
    ("abi_version", ctypes.c_uint32), ("struct_bytes", ctypes.c_uint32),
    ("market", ctypes.c_char_p), ("create", _Create), ("start", _Start),
    ("request_stop", _Stop), ("join", _Join), ("status_json", _Status),
    ("destroy", _Destroy),
]


class RecoveryRuntimeCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parser = argparse.ArgumentParser(add_help=False)
        parser.add_argument("sze_binary", nargs="?")
        parser.add_argument("sze_lib", nargs="?")
        parser.add_argument("fixture_binary", nargs="?")
        args, _ = parser.parse_known_args()
        paths = (args.sze_binary or os.environ.get("SZE_BINARY"),
                 args.sze_lib or os.environ.get("SZE_LIB"),
                 args.fixture_binary or os.environ.get("FIXTURE_BINARY"))
        if any(not path for path in paths):
            raise unittest.SkipTest("set SZE_BINARY, SZE_LIB and FIXTURE_BINARY")
        cls.sze_binary = os.path.abspath(paths[0])
        cls.sze_lib = os.path.abspath(paths[1])
        cls.fixture_binary = os.path.abspath(paths[2])
        for path in (cls.sze_binary, cls.sze_lib, cls.fixture_binary):
            if not os.path.exists(path):
                raise unittest.SkipTest("missing runtime recovery artifact: " + path)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sze-runtime-recovery-")

    def tearDown(self):
        self.temp.cleanup()

    def fixture(self):
        directory = os.path.join(self.temp.name, "journal")
        os.mkdir(directory)
        result = subprocess.run([self.fixture_binary, directory],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=5, check=True)
        metadata = json.loads(result.stdout.decode("utf-8").splitlines()[-1])
        return directory, metadata

    def config(self, directory, metadata, recovery_input="journal"):
        runtime = {
            "market": "SZ",
            "trading_day": metadata["day"],
            "model_path": "/absent-fixtures/synthetic-not-loaded.bin",
            "sz_orderbook_mode": "hp-realtime",
            "global_params": {"offset": 1.0, "global_bias_factor": 1.0,
                               "position_base_line": 100000.0,
                               "position_limit": 1000.0},
            "sze_recovery_consumer": {
                "enabled": True, "allow_invalid_replay_for_analysis": False,
                "trading_enabled": False, "trading_day": metadata["day"],
                "source_id": metadata["source"],
                "journal_directory": directory, "journal_prefix": metadata["prefix"],
                "journal_segment_mb": 1,
                "journal_max_payload_bytes": metadata["payload_max"],
                "shm_path": os.path.join(self.temp.name, "events.shm"),
                "expected_generation": metadata["generation"],
            },
            "ins_params": {"000001.SZ": {
                "Date": metadata["day"], "Close": 10.0,
                "HistoryAmount": 8000000.0, "FreeShare": 10000000.0,
                "HpUpperPrice": 11.0, "HpLowerPrice": 9.0,
                "HistoryVolatility20d": 0.02,
                "static_position": 0, "last_position": 0,
            }},
        }
        config = unified_config.migrate_runtime(runtime, "ACCOUNT-RECOVERY")
        if recovery_input == "handoff":
            config["market_data"]["recovery"]["shm_path"] = metadata["shm_path"]
            return unified_config.bind_environment(config, "live")
        return unified_config.bind_environment(config, "replay", directory,
                                               "canonical-events", "exchange")

    def profile(self, directory, metadata, changes=None, recovery_input="journal"):
        config = self.config(directory, metadata, recovery_input)
        if changes:
            changes(config)
        value = prepare.make_profile(config, factors_only=True,
                                     recovery_input=recovery_input)
        path = os.path.join(self.temp.name, "profile.json")
        with open(path, "w") as stream:
            json.dump(value, stream, sort_keys=True)
        return path, value

    def cli(self, action, directory, profile):
        return subprocess.run([self.sze_binary, action, directory, profile],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=10)

    @staticmethod
    def last_json(result):
        lines = result.stdout.decode("utf-8", "replace").splitlines()
        for line in reversed(lines):
            try:
                return json.loads(line)
            except ValueError:
                continue
        return None

    def api(self):
        library = ctypes.CDLL(self.sze_lib)
        entry = library.t0_market_runtime_v1
        entry.restype = ctypes.POINTER(_Api)
        api = entry().contents
        self.assertEqual(1, api.abi_version)
        return api

    def cabi(self, action, directory, profile, stop=False, timeout=10):
        api = self.api()
        error = ctypes.create_string_buffer(4096)
        handle = api.create(action.encode(), directory.encode(),
                            profile.encode(), error, len(error))
        self.assertTrue(handle, error.value.decode("utf-8", "replace"))
        try:
            self.assertEqual(1, api.start(handle, error, len(error)),
                             error.value.decode("utf-8", "replace"))
            deadline = time.monotonic() + timeout
            current = {}
            while time.monotonic() < deadline:
                size = api.status_json(handle, None, 0)
                if size:
                    buffer = ctypes.create_string_buffer(size)
                    api.status_json(handle, buffer, size)
                    current = json.loads(buffer.value.decode("utf-8"))
                if current.get("done") or (stop and current.get("ready")):
                    break
                time.sleep(0.005)
            if stop and current.get("ready"):
                api.request_stop(handle)
            self.assertEqual(1, api.join(handle, error, len(error)),
                             error.value.decode("utf-8", "replace"))
            size = api.status_json(handle, None, 0)
            buffer = ctypes.create_string_buffer(size)
            self.assertEqual(size, api.status_json(handle, buffer, size))
            return json.loads(buffer.value.decode("utf-8"))
        finally:
            api.destroy(handle)

    def start_handoff_fixture(self):
        directory = os.path.join(self.temp.name, "handoff")
        os.mkdir(directory)
        process = subprocess.Popen(
            [self.fixture_binary, "--handoff", directory],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop_handoff_fixture, process)
        ready, _, _ = select.select([process.stdout], [], [], 5)
        self.assertTrue(ready, "handoff fixture did not publish metadata")
        metadata = json.loads(process.stdout.readline().decode("utf-8"))
        return process, directory, metadata

    @staticmethod
    def stop_handoff_fixture(process):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)

    def test_cli_and_cabi_recovery_journal_match(self):
        directory, metadata = self.fixture()
        profile_path, _ = self.profile(directory, metadata)
        cli = self.cli("recovery-journal", directory, profile_path)
        self.assertEqual(0, cli.returncode, cli.stderr.decode("utf-8", "replace"))
        cli_result = self.last_json(cli)
        self.assertIsNotNone(cli_result)
        cabi_result = self.cabi("recovery-journal", directory, profile_path)
        for result in (cli_result, cabi_result):
            self.assertTrue(result["ok"])
            processing = result["processing"]
            self.assertGreater(processing["samples"], 0)
            self.assertEqual(0, processing["predictions"])
            self.assertEqual(6, processing["recovery"]["records"])
            self.assertTrue(processing["recovery"]["analysis_only"])
            self.assertFalse(processing["recovery"]["reached_live"])
            self.assertEqual("sze-journal", processing["recovery"]["driver"])
            self.assertNotIn("strategy", processing)
        self.assertEqual(cli_result["processing"], cabi_result["processing"])

    def test_recovery_rejects_generation_clock_and_recording_mismatch(self):
        directory, metadata = self.fixture()
        cases = (
            ("generation", lambda c: c["market_data"]["recovery"].update(
                {"expected_generation": metadata["generation"] + 1})),
            ("clock", lambda c: c["environment"].update({"time_basis": "recorded-receive"})),
            ("path", lambda c: c["environment"].update({
                "recording": os.path.join(self.temp.name, "other")})),
        )
        for name, change in cases:
            with self.subTest(case=name):
                try:
                    profile_path, _ = self.profile(directory, metadata, change)
                except unified_config.ConfigError:
                    continue
                result = self.cli("recovery-journal", directory, profile_path)
                self.assertNotEqual(0, result.returncode)

    def test_corrupt_journal_fails_without_samples_and_raw_replay_rejects_profile(self):
        directory, metadata = self.fixture()
        profile_path, _ = self.profile(directory, metadata)
        segment = os.path.join(directory, "fixture_20260904_s88_000000.szej")
        with open(segment, "r+b") as stream:
            stream.seek(4096 + 72 + 3)
            value = stream.read(1)
            stream.seek(4096 + 72 + 3)
            stream.write(bytes([value[0] ^ 0x01]))
        result = self.cli("recovery-journal", directory, profile_path)
        self.assertNotEqual(0, result.returncode)
        parsed = self.last_json(result)
        if parsed and "processing" in parsed:
            self.assertEqual(0, parsed["processing"]["samples"])

        raw = self.cli("replay", directory, profile_path)
        self.assertNotEqual(0, raw.returncode)

    def test_handoff_reaches_live_and_explicit_stop_joins(self):
        producer, directory, metadata = self.start_handoff_fixture()
        try:
            profile_path, _ = self.profile(directory, metadata,
                                           recovery_input="handoff")
            result = self.cabi("recovery-handoff", directory, profile_path,
                               stop=True, timeout=8)
            self.assertTrue(result["ok"])
            self.assertGreater(result["processing"]["recovery"]["records"], 0)
            self.assertFalse(result["processing"]["recovery"]["analysis_only"])
            self.assertTrue(result["processing"]["recovery"]["reached_live"])
            self.assertTrue(result["health"]["input_valid"])
            self.assertTrue(result["health"]["processing_valid"])
            self.assertFalse(result["health"]["recording_failed"])
        finally:
            self.stop_handoff_fixture(producer)

    def test_handoff_wrong_generation_rejects(self):
        producer, directory, metadata = self.start_handoff_fixture()
        try:
            def wrong(config):
                config["market_data"]["recovery"]["expected_generation"] += 1
            profile_path, _ = self.profile(directory, metadata, wrong,
                                           recovery_input="handoff")
            result = self.cli("recovery-handoff", directory, profile_path)
            self.assertNotEqual(0, result.returncode)
        finally:
            self.stop_handoff_fixture(producer)

    def test_handoff_loses_live_health_when_producer_exits(self):
        producer, directory, metadata = self.start_handoff_fixture()
        try:
            profile_path, _ = self.profile(directory, metadata,
                                           recovery_input="handoff")
            api = self.api()
            error = ctypes.create_string_buffer(4096)
            handle = api.create(b"recovery-handoff", directory.encode(),
                                profile_path.encode(), error, len(error))
            self.assertTrue(handle, error.value.decode("utf-8", "replace"))
            try:
                self.assertEqual(1, api.start(handle, error, len(error)))
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline:
                    size = api.status_json(handle, None, 0)
                    buffer = ctypes.create_string_buffer(size)
                    if size:
                        api.status_json(handle, buffer, size)
                        status = json.loads(buffer.value.decode("utf-8"))
                        ready = status.get("ready", False)
                        if ready:
                            break
                    time.sleep(0.005)
                self.assertTrue(ready)
                self.stop_handoff_fixture(producer)
                self.assertEqual(0, api.join(handle, error, len(error)))
                size = api.status_json(handle, None, 0)
                buffer = ctypes.create_string_buffer(size)
                api.status_json(handle, buffer, size)
                status = json.loads(buffer.value.decode("utf-8"))
                self.assertFalse(status.get("ready", True))
            finally:
                api.destroy(handle)
                producer = None
        finally:
            if producer is not None:
                self.stop_handoff_fixture(producer)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sze_binary")
    parser.add_argument("sze_lib")
    parser.add_argument("fixture_binary")
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RecoveryRuntimeCliTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
