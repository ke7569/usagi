#!/usr/bin/env python3
"""Offline integration tests for the t0_md_stream command line tool."""

from __future__ import print_function

import json
import os
import resource
import signal
import shutil
import socket
import subprocess
import struct
import sys
import tempfile
import time
import unittest
import zlib


class MdStreamCliTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.binary = sys.argv[1] if len(sys.argv) > 1 else None
        cls.observer = sys.argv[2] if len(sys.argv) > 2 else None
        if not cls.binary:
            raise unittest.SkipTest("t0_md_stream binary argument is required")

    def setUp(self):
        self.directory = tempfile.mkdtemp(prefix="t0-md-stream-cli-")

    def tearDown(self):
        shutil.rmtree(self.directory, ignore_errors=True)

    def port(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        value = sock.getsockname()[1]
        sock.close()
        return value

    def config(self, recording, **fields):
        value = {
            "recording_directory": recording,
            "channels": [{"name": "offline", "group": "127.0.0.1",
                           "port": self.port(), "interface_ip": "127.0.0.1"}],
            "duration_ms": 100,
        }
        value.update(fields)
        path = os.path.join(self.directory, "config.json")
        with open(path, "w") as output:
            json.dump(value, output)
        return path

    def invoke(self, *args, **kwargs):
        expected = kwargs.pop("expected", 0)
        process = subprocess.Popen([self.binary] + list(args),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
            self.fail("command timed out: %s" % self.binary)
        if process.returncode != expected:
            detail = stderr.decode("utf-8", "replace")[-1200:]
            self.fail("command returned %d (expected %d): %s" %
                      (process.returncode, expected, detail))
        if not stdout.strip():
            return None
        try:
            return json.loads(stdout.decode("utf-8").splitlines()[-1])
        except (ValueError, UnicodeDecodeError) as error:
            self.fail("invalid CLI JSON output: %s" % error)

    def capture_empty(self, recording):
        result = self.invoke("capture", self.config(recording))
        self.assertTrue(result["ok"])
        self.assertEqual(result["datagrams"], 0)
        self.assertTrue(result["stats"]["clean_recording"])
        self.assertTrue(os.path.isdir(recording))
        segment = os.path.join(recording, "stream_000000.t0md")
        self.assertTrue(os.path.isfile(segment))
        return segment

    def test_capture_empty_and_replay_clean_recording(self):
        recording = os.path.join(self.directory, "new-recording")
        self.capture_empty(recording)
        result = self.invoke("replay", recording)
        self.assertTrue(result["ok"])
        self.assertEqual(result["datagrams"], 0)
        self.assertTrue(result["stats"]["clean_recording"])

    def test_capture_rejects_existing_recording_directory(self):
        recording = os.path.join(self.directory, "already-there")
        os.mkdir(recording)
        self.invoke("capture", self.config(recording), expected=1)

    def test_config_rejects_unknown_and_invalid_fields(self):
        cases = [
            {"unknown": 1},
            {"duration_ms": True},
            {"receive_cpu": True},
            {"receive_cpu": 18446744073709551615},
            {"recording_directory": ""},
            {"recording_required": "false"},
        ]
        for index, fields in enumerate(cases):
            with self.subTest(case=index):
                recording = os.path.join(self.directory, "invalid-%d" % index)
                self.invoke("capture", self.config(recording, **fields), expected=1)

    def test_duplicate_config_key_is_rejected(self):
        path = os.path.join(self.directory, "duplicate.json")
        with open(path, "w") as output:
            output.write('{"recording_required":true,"recording_required":false}')
        self.invoke("capture", path, expected=1)

    def test_truncated_empty_recording_replay_fails_without_datagrams(self):
        recording = os.path.join(self.directory, "truncated")
        segment = self.capture_empty(recording)
        with open(segment, "r+b") as stream:
            stream.seek(0, os.SEEK_END)
            stream.truncate(stream.tell() - 1)
        result = self.invoke("replay", recording, expected=1)
        self.assertFalse(result["ok"])
        self.assertEqual(result["datagrams"], 0)

    def test_corrupt_recording_header_replay_fails_without_datagrams(self):
        recording = os.path.join(self.directory, "corrupt-header")
        segment = self.capture_empty(recording)
        with open(segment, "r+b") as stream:
            stream.seek(0)
            original = stream.read(1)
            stream.seek(0)
            stream.write(bytes(bytearray([original[0] ^ 1])))
        result = self.invoke("replay", recording, expected=1)
        self.assertFalse(result["ok"])
        self.assertEqual(result["datagrams"], 0)

    def test_sparse_extra_segment_is_rejected(self):
        recording = os.path.join(self.directory, "sparse")
        segment = self.capture_empty(recording)
        shutil.copyfile(segment, os.path.join(recording, "stream_000002.t0md"))
        self.assertFalse(self.invoke("replay", recording, expected=1)["ok"])

    def test_crc_valid_end_with_datagram_fields_is_rejected(self):
        recording = os.path.join(self.directory, "bad-end")
        segment = self.capture_empty(recording)
        with open(segment, "r+b") as stream:
            stream.seek(64 + 104)
            event = bytearray(stream.read(72))
            struct.pack_into("<I", event, 48, 1)
            struct.pack_into("<I", event, 12, 0)
            struct.pack_into("<I", event, 12, zlib.crc32(event) & 0xffffffff)
            stream.seek(64 + 104)
            stream.write(event)
        result = self.invoke("replay", recording, expected=1)
        self.assertEqual(result["datagrams"], 0)
        self.assertIn("control", result["error"])

    def test_observer_dev_full_fails_after_datagram(self):
        if not self.observer:
            self.skipTest("observer binary argument not supplied")
        port = self.port()
        process = subprocess.Popen(
            [self.observer, "/dev/full", "offline", "127.0.0.1", str(port),
             "--interface-ip", "127.0.0.1", "--duration-ms", "250"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            time.sleep(0.1)
            sender.sendto(b"offline-test", ("127.0.0.1", port))
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            self.fail("observer timed out")
        finally:
            sender.close()
        self.assertNotEqual(process.returncode, 0,
                            stderr.decode("utf-8", "replace")[-1200:])

    def test_writer_file_limit_failure_is_reported(self):
        self.check_writer_file_limit_failure(True)

    def test_optional_writer_failure_keeps_processing(self):
        self.check_writer_file_limit_failure(False)

    def check_writer_file_limit_failure(self, required):
        recording = os.path.join(self.directory, "write-failure")
        port = self.port()
        config = self.config(recording, duration_ms=500, recording_required=required,
            channels=[{"name": "offline", "group": "127.0.0.1", "port": port}],
            queue_capacity=128, max_datagram_bytes=1024, flush_interval_ms=10)

        def limit_file():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        process = subprocess.Popen([self.binary, "capture", config], preexec_fn=limit_file,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            time.sleep(0.12)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                for _ in range(20):
                    sender.sendto(b"x" * 100, ("127.0.0.1", port))
                time.sleep(0.1)
                for _ in range(20):
                    sender.sendto(b"y" * 100, ("127.0.0.1", port))
            stdout, stderr = process.communicate(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        self.assertEqual(process.returncode, 1 if required else 0)
        result = json.loads(stdout.decode())
        self.assertFalse(result["stats"]["clean_recording"])
        self.assertTrue(result["health"]["recording_failed"])
        self.assertTrue(result["health"]["input_valid"])
        self.assertTrue(result["health"]["processing_valid"])
        self.assertEqual(result["health"]["permits_new_risk"], not required)
        if not required:
            self.assertEqual(result["datagrams"], 40)
        self.assertIn("write", stderr.decode())
        replay = self.invoke("replay", recording, expected=1)
        self.assertEqual(replay["datagrams"], 0)

    def test_optional_recording_setup_failure_is_visible(self):
        recording = os.path.join(self.directory, "already-exists")
        os.mkdir(recording)
        result = self.invoke("capture", self.config(recording, recording_required=False))
        self.assertTrue(result["ok"])
        self.assertTrue(result["health"]["recording_failed"])
        self.assertTrue(result["health"]["input_valid"])
        self.assertIn("recording_error", result)
        self.assertEqual(os.listdir(recording), [])

    def test_sigterm_closes_recording_cleanly(self):
        recording = os.path.join(self.directory, "stop-signal")
        process = subprocess.Popen([self.binary, "capture", self.config(recording, duration_ms=0)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            for _ in range(100):
                if os.path.isfile(os.path.join(recording, "stream_000000.t0md")):
                    break
                time.sleep(0.01)
            process.terminate()
            stdout, stderr = process.communicate(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        self.assertEqual(process.returncode, 0, stderr.decode())
        self.assertTrue(json.loads(stdout.decode())["stats"]["clean_recording"])
        self.assertTrue(self.invoke("replay", recording)["ok"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
