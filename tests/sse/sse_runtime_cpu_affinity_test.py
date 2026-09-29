#!/usr/bin/env python3
"""Shanghai runtime CPU/L3 lease integration checks.

The native runtime is exercised in a child process so a failed affinity call
cannot leave the unittest process with altered thread state.  The test is
Linux-only because it verifies the kernel thread masks and CPU cache topology.
"""

from __future__ import print_function

import ctypes
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
import unittest


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
INTEGRATION = os.path.join(ROOT, "tests", "integration")
if INTEGRATION not in sys.path:
    sys.path.insert(0, INTEGRATION)

if sys.version_info[0] >= 3:
    import test_market_processing_cli as market_cli
    import test_market_runtime_api as runtime_api
else:
    # The checked-in integration fixture imports importlib.util and therefore
    # requires Python 3.  Keep the test runnable on the deployment host's
    # Python 2.7 without changing that shared fixture.
    market_cli = None
    runtime_api = None


CLI_ARGS = ()

try:
    integer_types = (int, long)
except NameError:
    integer_types = (int,)


def _decode(value):
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return value


def _python2_path_bytes(value):
    """Compatibility for the existing ctypes helper on Python 2.7."""
    if value is None:
        return None
    if isinstance(value, bytes):
        return value
    return value.encode(sys.getfilesystemencoding() or "utf-8")


if not hasattr(os, "fsencode"):
    if runtime_api is not None:
        runtime_api.encoded = _python2_path_bytes


def _write_json(path, value):
    with open(path, "w") as output:
        json.dump(value, output, sort_keys=True)


def _port():
    import socket
    handle = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    handle.bind(("127.0.0.1", 0))
    value = handle.getsockname()[1]
    handle.close()
    return value


def _fallback_profile(path):
    """Minimal SH factors-only profile for Python 2 deployment hosts."""
    value = {
        "schema_version": 1,
        "market": "SH",
        "execution": "disabled",
        "processing_mode": "factors-only",
        "processing_contract": "sse-per-instrument-v2",
        "trading_day": 20260904,
        "processing_sha256": "0" * 64,
        "environment": {"mode": "live", "clock": "host", "execution": "disabled"},
        "prediction": {},
        "instruments": [{
            "instrument": "600000",
            "trading_date": 20260904,
            "average_amount": 8000000.0,
            "turnover_threshold": 1000.0,
            "free_share": 10000000.0,
            "pre_close": 10.0,
            "upper_limit": 11.0,
            "lower_limit": 9.0,
        }],
    }
    _write_json(path, value)


def _fallback_stream(path, recording):
    value = {
        "channels": [{"name": "loopback", "group": "127.0.0.1",
                      "interface_ip": "127.0.0.1", "port": _port()}],
        "recording_directory": recording,
        "recording_required": True,
        "duration_ms": 0,
        "queue_capacity": 4096,
        "max_datagram_bytes": 1024,
        "receive_batch_size": 64,
        "receive_buffer_bytes": 1048576,
        "idle_gap_ns": 100000,
        "segment_bytes": 32768,
        "flush_interval_ms": 5,
        "receive_cpu": -1,
        "dispatch_cpu": -1,
        "writer_cpu": -1,
    }
    _write_json(path, value)
    return value


if runtime_api is None:
    CharBuffer = ctypes.POINTER(ctypes.c_char)
    Create = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                              ctypes.c_char_p, CharBuffer, ctypes.c_size_t)
    Start = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, CharBuffer, ctypes.c_size_t)
    RequestStop = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
    Join = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, CharBuffer, ctypes.c_size_t)
    StatusJson = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p, CharBuffer, ctypes.c_size_t)
    Destroy = ctypes.CFUNCTYPE(None, ctypes.c_void_p)

    class _RuntimeApiV1(ctypes.Structure):
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

    class _LoadedRuntime(object):
        def __init__(self, path):
            mode = getattr(ctypes, "RTLD_LOCAL", 0)
            self.library = ctypes.CDLL(path, mode=mode)
            getter = self.library.t0_market_runtime_v1
            getter.argtypes = []
            getter.restype = ctypes.POINTER(_RuntimeApiV1)
            pointer = getter()
            if not pointer:
                raise AssertionError("null runtime API")
            self.api = pointer.contents

        def create(self, action, stream, profile):
            error = ctypes.create_string_buffer(4096)
            handle = self.api.create(_python2_path_bytes(action), _python2_path_bytes(stream),
                                     _python2_path_bytes(profile), error, len(error))
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
            needed = self.api.status_json(handle, None, 0)
            output = ctypes.create_string_buffer(needed)
            self.api.status_json(handle, output, len(output))
            return json.loads(output.value.decode("utf-8"))

        def destroy(self, handle):
            self.api.destroy(handle)

    def _load_runtime(path):
        return _LoadedRuntime(path)
else:
    def _load_runtime(path):
        return runtime_api.LoadedRuntime(path)


def _read(path):
    with open(path, "r") as source:
        return source.read().strip()


def _parse_cpu_list(value):
    result = []
    value = value.strip()
    if not value:
        raise ValueError("empty CPU list")
    for part in value.split(","):
        bounds = part.split("-")
        if len(bounds) > 2 or not bounds[0]:
            raise ValueError("invalid CPU list: " + value)
        first = int(bounds[0])
        last = first if len(bounds) == 1 else int(bounds[1])
        if first > last:
            raise ValueError("descending CPU range: " + value)
        result.extend(range(first, last + 1))
    return result


def _task_ids(pid):
    root = "/proc/{}/task".format(pid)
    return set(int(name) for name in os.listdir(root) if name.isdigit())


def _proc_affinity(tid):
    """Return the kernel mask used by sched_getaffinity, with Python 2 fallback."""
    if hasattr(os, "sched_getaffinity"):
        try:
            return set(os.sched_getaffinity(tid))
        except OSError:
            pass
    status = "/proc/{}/task/{}/status".format(os.getpid(), tid)
    try:
        for line in open(status, "r"):
            if line.startswith("Cpus_allowed_list:"):
                return set(_parse_cpu_list(line.split(":", 1)[1]))
    except (IOError, OSError, ValueError):
        return None
    return None


def _cpu_topology():
    """Return (domain -> allowed CPUs, CPU -> exact C++ domain string)."""
    allowed = _proc_affinity(os.getpid())
    if not allowed:
        raise unittest.SkipTest("cannot read this process CPU affinity")
    try:
        online = set(_parse_cpu_list(_read("/sys/devices/system/cpu/online")))
    except (IOError, OSError, ValueError):
        raise unittest.SkipTest("cannot read online CPU topology from sysfs")
    # Some hosts expose possible/offline CPUs in /proc affinity masks.  The
    # lease can only bind CPUs that have a sysfs cache topology.
    allowed &= online
    if not allowed:
        raise unittest.SkipTest("the process has no online CPUs in its affinity mask")
    domains = {}
    by_cpu = {}
    missing = []
    for cpu in sorted(allowed):
        cache_root = "/sys/devices/system/cpu/cpu{}/cache".format(cpu)
        found = None
        try:
            entries = os.listdir(cache_root)
        except OSError:
            entries = []
        for entry in entries:
            cache = os.path.join(cache_root, entry)
            try:
                if _read(os.path.join(cache, "level")) != "3":
                    continue
                members = sorted(set(_parse_cpu_list(_read(os.path.join(cache, "shared_cpu_list")))))
            except (IOError, OSError, ValueError):
                continue
            if members:
                # This is the same canonical spelling produced by sse_cpu::domain().
                found = ",".join(str(member) for member in members)
                break
        if not found:
            missing.append(cpu)
            continue
        by_cpu[cpu] = found
        domains.setdefault(found, []).append(cpu)
    if missing:
        raise unittest.SkipTest("sysfs has no usable L3 for allowed CPU(s): {}".format(missing))
    if len(domains) < 3:
        raise unittest.SkipTest("requires at least three allowed L3 domains; found {}".format(len(domains)))
    return domains, by_cpu


def _singleton_cpus(before):
    """Read singleton affinity masks for runtime threads created after `before`."""
    result = {}
    try:
        current = _task_ids(os.getpid())
    except (IOError, OSError):
        return result
    for tid in sorted(current - before):
        mask = _proc_affinity(tid)
        if mask and len(mask) == 1:
            result[tid] = next(iter(mask))
    return result


def _wait_ready(runtime, handle):
    deadline = time.time() + 5.0
    while time.time() < deadline:
        status = runtime.status(handle)
        if status.get("ready"):
            return status
        if status.get("done"):
            raise AssertionError("runtime stopped before ready: {}".format(status))
        time.sleep(0.01)
    raise AssertionError("runtime did not report readiness")


def _wait_bound_threads(before):
    deadline = time.time() + 5.0
    observed = {}
    while time.time() < deadline:
        observed.update(_singleton_cpus(before))
        if len(set(observed.values())) >= 3:
            return observed
        time.sleep(0.01)
    raise AssertionError("did not observe three bound runtime threads: {}".format(observed))


def _plan(status, by_cpu):
    value = status.get("cpu_affinity")
    if not isinstance(value, list):
        raise AssertionError("final status has no cpu_affinity array: {}".format(status))
    if len(value) != 3:
        raise AssertionError("expected receive/dispatch/writer CPU plan: {}".format(value))
    roles = {}
    for entry in value:
        if not isinstance(entry, dict) or set(entry) != set(("role", "cpu", "l3")):
            raise AssertionError("invalid cpu_affinity entry: {}".format(entry))
        role = entry["role"]
        if role in roles or role not in ("receive", "dispatch", "writer"):
            raise AssertionError("invalid or duplicate CPU role: {}".format(role))
        cpu = entry["cpu"]
        if not isinstance(cpu, integer_types) or isinstance(cpu, bool):
            raise AssertionError("invalid CPU id: {}".format(entry))
        if cpu not in by_cpu or entry["l3"] != by_cpu[cpu]:
            raise AssertionError("status L3 does not match sysfs for {}".format(entry))
        roles[role] = {"cpu": cpu, "l3": entry["l3"]}
    if set(roles) != set(("receive", "dispatch", "writer")):
        raise AssertionError("missing CPU role: {}".format(roles))
    if len(set(item["cpu"] for item in roles.values())) != 3:
        raise AssertionError("roles do not have distinct CPUs: {}".format(roles))
    if len(set(item["l3"] for item in roles.values())) != 3:
        raise AssertionError("roles do not have distinct L3 domains: {}".format(roles))
    return roles


def _assert_threads_match(plan, observed):
    actual = set(observed.values())
    expected = set(item["cpu"] for item in plan.values())
    if not expected.issubset(actual):
        raise AssertionError("status plan CPUs were not singleton-affine runtime threads; expected {}, observed {}"
                             .format(sorted(expected), sorted(actual)))


class Scenario(object):
    def __init__(self, library):
        self.runtime = _load_runtime(library)
        self.fixture = market_cli.MarketProcessingCliTests() if market_cli is not None else None

    def _inputs(self, root, name, cpus=None):
        directory = os.path.join(root, name)
        os.mkdir(directory)
        recording = os.path.join(directory, "recording")
        if self.fixture is not None:
            profile, unused_bound = self.fixture.bound_profile(recording, "SH", factors_only=True)
            stream = self.fixture.stream_config(recording, market_cli.port(), 100000)
            stream["duration_ms"] = 0
        else:
            profile = os.path.join(directory, "SH-profile.json")
            _fallback_profile(profile)
            stream = _fallback_stream(os.path.join(directory, "stream.json"), recording)
        if cpus is not None:
            stream["receive_cpu"], stream["dispatch_cpu"], stream["writer_cpu"] = cpus
        stream_path = os.path.join(directory, "stream.json")
        if self.fixture is not None:
            market_cli.write_json(stream_path, stream)
        else:
            _write_json(stream_path, stream)
        return stream_path, profile

    def _capture(self, root, name, by_cpu, cpus=None):
        stream, profile = self._inputs(root, name, cpus)
        handle, error = self.runtime.create("capture", stream, profile)
        if not handle:
            raise AssertionError("capture create failed: {}".format(error))
        before = _task_ids(os.getpid())
        try:
            started, error = self.runtime.start(handle)
            if not started:
                raise AssertionError("capture start failed: {}".format(error))
            _wait_ready(self.runtime, handle)
            observed = _wait_bound_threads(before)
            self.runtime.api.request_stop(handle)
            joined, error = self.runtime.join(handle)
            if not joined:
                raise AssertionError("capture join failed: {}".format(error))
            status = self.runtime.status(handle)
            if not status.get("done") or not status.get("ok"):
                raise AssertionError("capture did not finish cleanly: {}".format(status))
            plan = _plan(status, by_cpu)
            _assert_threads_match(plan, observed)
            return plan
        finally:
            self.runtime.api.request_stop(handle)
            if hasattr(self.runtime, "destroy"):
                self.runtime.destroy(handle)
            else:
                self.runtime.api.destroy(handle)

    def auto_then_reuse(self, root, by_cpu):
        first = self._capture(root, "auto", by_cpu)
        requested = tuple(first[role]["cpu"] for role in ("receive", "dispatch", "writer"))
        second = self._capture(root, "reuse", by_cpu, requested)
        if second != first:
            raise AssertionError("released CPU lease was not reusable: first={}, second={}".format(first, second))

    def reject_same_domain(self, root, domains, by_cpu):
        domain = sorted(domains)[0]
        members = sorted(domains[domain])
        if len(members) >= 3:
            requested = tuple(members[:3])
        elif len(members) == 2:
            requested = (members[0], members[1], members[0])
        else:
            requested = (members[0], members[0], members[0])
        stream, profile = self._inputs(root, "same-l3", requested)
        handle, create_error = self.runtime.create("capture", stream, profile)
        if not handle:
            if "L3" not in create_error and "CPU" not in create_error and "domain" not in create_error:
                raise AssertionError("same-domain create failed for an unrelated reason: {}".format(create_error))
            return
        try:
            started, start_error = self.runtime.start(handle)
            if started:
                joined, join_error = self.runtime.join(handle)
                error = join_error or self.runtime.status(handle).get("error", "")
                if joined:
                    raise AssertionError("same-L3 capture unexpectedly succeeded")
            else:
                error = start_error
            if not any(word in error for word in ("L3", "CPU", "domain")):
                raise AssertionError("same-domain rejection lacked an affinity error: {}".format(error))
        finally:
            self.runtime.api.request_stop(handle)
            if hasattr(self.runtime, "destroy"):
                self.runtime.destroy(handle)
            else:
                self.runtime.api.destroy(handle)


def _worker(case, library):
    domains, by_cpu = _cpu_topology()
    root = tempfile.mkdtemp(prefix="sse-runtime-cpu-")
    try:
        scenario = Scenario(library)
        if case == "auto_then_reuse":
            scenario.auto_then_reuse(root, by_cpu)
        elif case == "reject_same_domain":
            scenario.reject_same_domain(root, domains, by_cpu)
        else:
            raise AssertionError("unknown worker case: {}".format(case))
    finally:
        shutil.rmtree(root)


class RuntimeCpuAffinityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if len(CLI_ARGS) != 1:
            raise unittest.SkipTest("usage: sse_runtime_cpu_affinity_test.py SSE_RUNTIME_LIB")
        if not os.path.isfile(CLI_ARGS[0]):
            raise unittest.SkipTest("runtime library does not exist: {}".format(CLI_ARGS[0]))
        cls.domains, cls.by_cpu = _cpu_topology()

    def isolated(self, case):
        command = [sys.executable, "-B", os.path.abspath(__file__), "--worker", case, CLI_ARGS[0]]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.time() + 30.0
        while process.poll() is None and time.time() < deadline:
            time.sleep(0.05)
        if process.poll() is None:
            process.kill()
        output, error = process.communicate()
        detail = _decode(output) + _decode(error)
        if process.returncode != 0:
            self.fail("{} failed ({}):\n{}".format(case, process.returncode, detail))

    def test_auto_affinity_and_stop_release_reuse(self):
        self.isolated("auto_then_reuse")

    def test_explicit_same_l3_is_rejected(self):
        self.isolated("reject_same_domain")


def _main():
    if len(sys.argv) > 1 and sys.argv[1] == "--worker":
        if len(sys.argv) != 4:
            print("usage: --worker CASE SSE_RUNTIME_LIB", file=sys.stderr)
            return 2
        try:
            _worker(sys.argv[2], sys.argv[3])
            return 0
        except unittest.SkipTest as error:
            print("SKIP: {}".format(error))
            return 0
        except Exception:
            traceback.print_exc()
            return 1
    global CLI_ARGS
    CLI_ARGS = tuple(sys.argv[1:])
    sys.argv = sys.argv[:1]
    return unittest.main(verbosity=2)


if __name__ == "__main__":
    result = _main()
    if isinstance(result, int):
        sys.exit(result)
