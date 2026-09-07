#!/usr/bin/env python3
"""Contract checks for the normal and profiled OMS microbenchmarks."""

import json
import os
import subprocess
import sys
import tempfile
import unittest


class BenchmarkContractTest(unittest.TestCase):
    def run_binary(self, binary, expected_build, root, audit_text=False):
        command = [binary, "--repetitions", "1", "--orders", "16",
                   "--durable-orders", "2", "--journal-root", root]
        if audit_text:
            command += ["--audit-text", "1"]
        process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        stdout, stderr = process.stdout, process.stderr
        if process.returncode:
            self.fail("benchmark failed: %s\nstdout:\n%s\nstderr:\n%s" %
                      (" ".join(command), stdout.decode("utf-8", "replace"),
                       stderr.decode("utf-8", "replace")))
        rows = []
        for number, line in enumerate(stdout.decode("utf-8").splitlines(), 1):
            try:
                rows.append(json.loads(line))
            except ValueError as error:
                self.fail("invalid JSON line %d from %s: %s\n%s" %
                          (number, binary, error, line))
        self.assertEqual("metadata", rows[0].get("kind"), rows)
        self.assertEqual(expected_build, rows[0].get("build"), rows)
        runs = [row for row in rows if row.get("kind") == "run"]
        self.assertEqual(5 if expected_build == "normal" else 7, len(runs), rows)
        self.assertEqual(1, rows[0]["repetitions"])
        self.assertEqual(16, rows[0]["orders"])
        self.assertEqual(2, rows[0]["durable_orders"])
        self.assertFalse(rows[0]["intent_construction_timed"])
        self.assertFalse(rows[0]["timing_overhead_subtracted"])
        for row in runs:
            self.assertIn(row["path"], ("direct_paper", "memory", "durable"), row)
            self.assertEqual(32, row["warmup"], row)
            self.assertEqual(16 if row["path"] != "durable" else 2,
                             row["count"], row)
            latency = row["latency_ns"]
            self.assertIn("total", latency, row)
            self.assertNotIn("stages", latency, row)
            for metric in latency.values():
                for key in ("p50", "p95", "p99", "max", "mean"):
                    self.assertGreaterEqual(metric[key], 0, row)
            if row["measurement"] == "total":
                self.assertNotIn("pre_backend", latency, row)
                self.assertNotIn("backend", latency, row)
                self.assertNotIn("post_backend", latency, row)
            else:
                for key in ("pre_backend", "backend", "post_backend"):
                    self.assertIn(key, latency, row)
                self.assertAlmostEqual(latency["total"]["mean"],
                                       latency["pre_backend"]["mean"] +
                                       latency["backend"]["mean"] +
                                       latency["post_backend"]["mean"],
                                       delta=0.01, msg=row)
            if row["path"] != "direct_paper":
                state = row["state"]
                self.assertEqual(audit_text, row["audit_text"], row)
                if audit_text:
                    self.assertEqual(state["journal_records"], row["audit"]["records"], row)
                    self.assertGreater(row["audit"]["bytes"], 0, row)
                self.assertEqual(row["count"] + row["warmup"], state["orders"], row)
                self.assertEqual(state["orders"], state["pending"], row)
                self.assertEqual(state["orders"], state["timers"], row)
                self.assertEqual(0, state["actions"], row)
                self.assertEqual(state["orders"] * 100000 * 100,
                                 state["reserved"], row)
                self.assertEqual(state["orders"] * 100,
                                 state["working_buy"], row)
                self.assertEqual(10000000, state["position"], row)
                self.assertEqual(10000000, state["sellable"], row)
                self.assertGreaterEqual(state["audit_retained"], 1, row)
                self.assertEqual(32, row["warmup"], row)
                if row["measurement"] == "detailed":
                    self.assertIn("stages", row)
                    stages = row["stages"]
                    self.assertEqual(row["count"], stages["submit"]["calls"], row)
                    self.assertEqual(3 * row["count"], stages["journal_append"]["calls"], row)
                    self.assertAlmostEqual(sum(s["exclusive_ns"]["mean"] for s in stages.values()),
                                           stages["submit"]["inclusive_ns"]["mean"], delta=0.01, msg=row)
                    if row["path"] == "memory":
                        self.assertEqual(0, stages["journal_sync"]["calls"], row)
                    else:
                        self.assertEqual(row["count"], stages["journal_sync"]["calls"], row)
                else:
                    self.assertNotIn("stages", row)
        return rows

    def test_actual_async_text_output(self):
        if len(sys.argv) != 3:
            self.skipTest("usage: test_benchmark_contract.py normal_binary profiled_binary")
        with tempfile.TemporaryDirectory(prefix="usagi-oms-async-benchmark-test-") as root:
            for binary, build in zip(sys.argv[1:], ("normal", "profiled")):
                without = self.run_binary(binary, build, root)
                with_text = self.run_binary(binary, build, root, audit_text=True)
                self.assertEqual([row.get("state") for row in without],
                                 [row.get("state") for row in with_text])
                self.assertEqual([], os.listdir(root))

    def test_normal_and_profiled_contract(self):
        if len(sys.argv) != 3:
            self.skipTest("usage: test_benchmark_contract.py normal_binary profiled_binary")
        normal, profiled = sys.argv[1:]
        all_states = {}
        with tempfile.TemporaryDirectory(prefix="usagi-oms-benchmark-test-") as root:
            for binary, build in ((normal, "normal"), (profiled, "profiled")):
                before = set(os.listdir(root))
                rows = self.run_binary(binary, build, root)
                after = set(os.listdir(root))
                self.assertEqual(before, after, (binary, before, after))
                for row in rows:
                    if row.get("kind") == "run" and row["path"] in ("memory", "durable"):
                        key = (row["path"], row["count"])
                        state = row["state"]
                        if key in all_states:
                            self.assertEqual(all_states[key], state, (binary, row))
                        else:
                            all_states[key] = state

    def test_rejects_invalid_arguments(self):
        if len(sys.argv) != 3:
            self.skipTest("usage: test_benchmark_contract.py normal_binary profiled_binary")
        for binary in sys.argv[1:]:
            for args in (("--orders", "0"), ("--cpu", "1024"), ("--unknown", "1")):
                process = subprocess.run([binary] + list(args), stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE, timeout=10)
                stdout, stderr = process.stdout, process.stderr
                self.assertNotEqual(0, process.returncode,
                                    (binary, args, stdout, stderr))

    def test_focused_probe(self):
        if len(sys.argv) != 3:
            self.skipTest("usage: test_benchmark_contract.py normal_binary profiled_binary")
        output = subprocess.run([sys.argv[2], "--repetitions", "1", "--orders", "16",
                                 "--mode", "memory", "--focus", "risk"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(0, output.returncode, output.stderr)
        rows = [json.loads(line) for line in output.stdout.decode().splitlines()]
        total = next(row for row in rows if row.get("path") == "memory" and row.get("measurement") == "total")
        focused = next(row for row in rows if row.get("measurement") == "focused")
        self.assertEqual(total["state"], focused["state"])
        self.assertEqual({"total"}, set(focused["latency_ns"]))
        self.assertEqual("risk", focused["focus"])
        self.assertEqual(16, focused["stages"]["risk"]["calls"])
        for name, stage in focused["stages"].items():
            if name != "risk":
                self.assertEqual(0, stage["calls"], name)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
