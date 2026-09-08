#!/usr/bin/env python
from __future__ import print_function
import datetime
import imp
import os
import shutil
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
retention = imp.load_source("capture_retention", os.path.join(ROOT, "tools/sse/prune_capture_days.py"))


class RetentionTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="usagi-retention-test-")
        self.old_argv = sys.argv
        self.old_available = retention.available

    def tearDown(self):
        retention.available = self.old_available
        sys.argv = self.old_argv
        shutil.rmtree(self.root)

    def directory(self, name):
        path = os.path.join(self.root, name)
        os.mkdir(path)
        with open(os.path.join(path, "record.szej"), "w") as output:
            output.write("synthetic retention fixture")
        return path

    def run_retention(self, delete=True, keep="20990101"):
        sys.argv = ["retention", "--root", self.root, "--keep-day", keep, "--min-free-gib", "1"]
        if delete:
            sys.argv.append("--delete")
        return retention.main()

    def test_space_available_does_not_remove_any_day(self):
        old = self.directory("20200101")
        retention.available = lambda path: 2 << 30
        self.assertEqual(0, self.run_retention())
        self.assertTrue(os.path.isdir(old))

    def test_only_oldest_day_removed_when_it_frees_enough_space(self):
        oldest = self.directory("20200101")
        newer = self.directory("20200102")
        retention.available = lambda path: 0 if os.path.exists(oldest) else 2 << 30
        self.assertEqual(0, self.run_retention())
        self.assertFalse(os.path.exists(oldest))
        self.assertTrue(os.path.isdir(newer))

    def test_current_future_and_symlink_days_are_preserved(self):
        today = self.directory(datetime.date.today().strftime("%Y%m%d"))
        future = self.directory("20980101")
        unrelated = self.directory("configuration")
        os.symlink(unrelated, os.path.join(self.root, "20200101"))
        retention.available = lambda path: 0
        self.assertEqual(1, self.run_retention())
        for path in (today, future, unrelated):
            self.assertTrue(os.path.isdir(path))
        self.assertTrue(os.path.islink(os.path.join(self.root, "20200101")))

    def test_dry_run_never_deletes(self):
        oldest = self.directory("20200101")
        retention.available = lambda path: 0
        self.assertEqual(1, self.run_retention(delete=False))
        self.assertTrue(os.path.isdir(oldest))


if __name__ == "__main__":
    unittest.main()
