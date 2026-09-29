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
        self.old_open = retention.open_days
        retention.open_days = lambda root: set()

    def tearDown(self):
        retention.open_days = self.old_open
        shutil.rmtree(self.root)

    def directory(self, name):
        path = os.path.join(self.root, name)
        os.makedirs(os.path.join(path, 'journal'))
        with open(os.path.join(path, 'journal', "record.szej"), "w") as output:
            output.write("synthetic retention fixture")
        with open(os.path.join(path, 'capture.stdout'), 'w') as output:
            output.write('{"ok":true,"stats":{"journal_clean":true}}')
        return path

    def run_retention(self, delete=True):
        argv = ["--root", self.root, "--keep-days", "5"]
        if delete:
            argv.append("--apply")
        return retention.main(argv)

    def extra_file(self, directory, relative):
        path = os.path.join(directory, relative)
        parent = os.path.dirname(path)
        if not os.path.isdir(parent):
            os.makedirs(parent)
        with open(path, 'w') as output:
            output.write('must preserve ' + relative)
        return path

    def test_fewer_than_five_days_are_preserved(self):
        old = self.directory("20200101")
        self.assertEqual(0, self.run_retention())
        self.assertTrue(os.path.isdir(old))

    def test_only_five_newest_days_are_retained(self):
        oldest = self.directory("20200101")
        newer = self.directory("20200102")
        for day in range(3, 8):
            self.directory('202001%02d' % day)
        self.assertEqual(0, self.run_retention())
        self.assertFalse(os.path.exists(oldest))
        self.assertFalse(os.path.exists(newer))
        self.assertEqual(['20200103', '20200104', '20200105', '20200106', '20200107'], sorted(os.listdir(self.root)))

    def test_old_layout_preserves_td_models_and_profile(self):
        oldest = self.directory('20190101')
        protected = [self.extra_file(oldest, name) for name in (
            'td-query/oms.journal', 'td-query/account.json', 'td_query.json',
            'model/prediction.log', 'profile/processing.json', 'config_daily.json')]
        self.extra_file(oldest, 'capture.json')
        for day in range(1, 6):
            self.directory('202001%02d' % day)
        self.assertEqual(0, self.run_retention())
        self.assertFalse(os.path.exists(os.path.join(oldest, 'journal')))
        self.assertFalse(os.path.exists(os.path.join(oldest, 'capture.stdout')))
        self.assertFalse(os.path.exists(os.path.join(oldest, 'capture.json')))
        for path in protected:
            with open(path) as source:
                self.assertTrue(source.read().startswith('must preserve '))
        # Metadata-only dates no longer consume one of the five capture slots.
        rows = retention.plan(self.root, 5, datetime.date.today(), opened=set())
        self.assertNotIn('20190101', [row['day'] for row in rows])

    @unittest.skipUnless(os.name != 'nt', 'requires Linux symlinks')
    def test_epoch_layout_preserves_other_day_and_epoch_material(self):
        oldest = os.path.join(self.root, '20190101')
        first = self.directory('20190101/capture-first')
        second = self.directory('20190101/capture-second')
        os.symlink(second, os.path.join(oldest, 'current'))
        protected = [self.extra_file(oldest, name) for name in (
            'td-query/oms.journal', 'profile/live.json', 'model/weights.bin')]
        protected.append(self.extra_file(first, 'manual_note.txt'))
        for day in range(1, 6):
            self.directory('202001%02d' % day)
        rows = retention.plan(self.root, 5, datetime.date.today(), opened=set())
        deletion = [row for row in rows if row['day'] == '20190101'][0]
        self.assertNotIn(oldest, deletion['targets'])
        self.assertNotIn(first, deletion['targets'])
        self.assertEqual(0, self.run_retention())
        self.assertFalse(os.path.exists(os.path.join(first, 'journal')))
        self.assertFalse(os.path.exists(second))
        self.assertFalse(os.path.lexists(os.path.join(oldest, 'current')))
        for path in protected:
            self.assertTrue(os.path.isfile(path))

    @unittest.skipUnless(os.name != 'nt', 'requires Linux symlinks')
    def test_active_epoch_and_open_td_day_remain_protected(self):
        active = self.directory('20190101/capture-active')
        opened = self.directory('20190102/capture-stopped')
        self.extra_file(os.path.dirname(opened), 'td-query/oms.journal')
        os.symlink(active, os.path.join(self.root, 'active_capture'))
        retention.open_days = lambda root: set(('20190102',))
        for day in range(1, 6):
            self.directory('202001%02d' % day)
        self.assertEqual(0, self.run_retention())
        self.assertTrue(os.path.isdir(os.path.join(active, 'journal')))
        self.assertTrue(os.path.isdir(os.path.join(opened, 'journal')))

    @unittest.skipUnless(os.name != 'nt', 'requires Linux symlinks')
    def test_current_future_and_symlink_days_are_preserved(self):
        today = self.directory(datetime.date.today().strftime("%Y%m%d"))
        future = self.directory("20980101")
        unrelated = self.directory("configuration")
        os.symlink(unrelated, os.path.join(self.root, "20200101"))
        self.assertEqual(0, self.run_retention())
        for path in (today, future, unrelated):
            self.assertTrue(os.path.isdir(path))
        self.assertTrue(os.path.islink(os.path.join(self.root, "20200101")))

    def test_dry_run_never_deletes(self):
        oldest = self.directory("20200101")
        for day in range(2, 8):
            self.directory('202001%02d' % day)
        self.assertEqual(0, self.run_retention(delete=False))
        self.assertTrue(os.path.isdir(oldest))

    @unittest.skipUnless(os.name != 'nt', 'requires Linux symlinks')
    def test_active_and_open_days_are_preserved(self):
        active = self.directory('20190101')
        opened = self.directory('20190102')
        incomplete = self.directory('20190103')
        os.unlink(os.path.join(incomplete, 'capture.stdout'))
        os.symlink(active, os.path.join(self.root, 'active_capture'))
        retention.open_days = lambda root: set(('20190102',))
        for day in range(1, 8):
            self.directory('202001%02d' % day)
        self.assertEqual(0, self.run_retention())
        for path in (active, opened):
            self.assertTrue(os.path.isdir(path))
        self.assertFalse(os.path.exists(incomplete))

    @unittest.skipUnless(os.name != 'nt', 'requires Linux symlinks')
    def test_nested_external_symlink_is_not_followed_or_removed(self):
        old = self.directory('20180101')
        outside = tempfile.mkdtemp(prefix='outside-retention-test-')
        try:
            os.symlink(outside, os.path.join(old, 'external'))
            for day in range(1, 8):
                self.directory('202001%02d' % day)
            self.assertEqual(0, self.run_retention())
            self.assertTrue(os.path.isdir(old))
            self.assertTrue(os.path.isdir(outside))
        finally:
            shutil.rmtree(outside)

    @unittest.skipUnless(os.path.isdir('/proc'), 'requires procfs')
    def test_real_open_file_detected(self):
        day = self.directory('20190101')
        with open(os.path.join(day, 'journal', 'record.szej'), 'r'):
            self.assertIn('20190101', self.old_open(self.root))

    def test_root_and_invalid_count_rejected(self):
        with self.assertRaises(ValueError):
            retention.plan('/', 5, datetime.date.today(), opened=set())
        with self.assertRaises(ValueError):
            retention.plan(self.root, 0, datetime.date.today(), opened=set())

    def test_calendar_gaps_and_current_count_as_one_of_five(self):
        for name in ('20191220', '20191223', '20191230', '20200102', '20200103', '20200106', '20200108'):
            self.directory(name)
        rows = retention.plan(self.root, 5, datetime.date(2020, 1, 8), opened=set())
        self.assertEqual(['20191220', '20191223'], [r['day'] for r in rows if r.get('action') == 'delete'])


if __name__ == "__main__":
    unittest.main()
