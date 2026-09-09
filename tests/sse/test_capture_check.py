#!/usr/bin/env python
from __future__ import print_function
import imp
import json
import os
import shutil
import tempfile
import unittest
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../..'))
check = imp.load_source('capture_check', os.path.join(ROOT, 'deploy/sse/check_journal_capture.py'))


class CheckTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='capture-check-test-')
        self.epoch = os.path.join(self.root, 'capture-test')
        os.makedirs(os.path.join(self.epoch, 'journal'))
        self.status = {'ready': True, 'input_valid': True, 'processing_valid': True,
                       'journal_errors': 0, 'journal_overflows': 0, 'journal_degraded': False,
                       'recording_failed': False, 'stop_requested': False,
                       'channels': [{'name': 'sse_tick', 'datagrams': 10},
                                    {'name': 'sse_snapshot', 'datagrams': 20}]}
        self.final = {'ok': True, 'accepted_events': 30, 'journal_events': 30,
                      'journal_errors': 0, 'journal_overflows': 0,
                      'channels': self.status['channels'],
                      'stats': {'journal_clean': True, 'kernel_drops': 0}}

    def tearDown(self):
        shutil.rmtree(self.root)

    def write(self, name, value):
        path = os.path.join(self.epoch, name)
        with open(path, 'w') as output:
            json.dump(value, output)
        return path

    def test_live_status_rejects_stale_failed_or_missing_channel(self):
        path = self.write('capture.stderr', self.status)
        self.assertEqual(self.status, check.live_status(self.epoch))
        os.utime(path, (1, 1))
        with self.assertRaises(ValueError):
            check.live_status(self.epoch)
        self.status['ready'] = False
        self.write('capture.stderr', self.status)
        with self.assertRaises(ValueError):
            check.live_status(self.epoch)
        self.status['ready'] = True
        self.status['channels'].pop()
        self.write('capture.stderr', self.status)
        with self.assertRaises(ValueError):
            check.live_status(self.epoch)

    def test_closed_checks_every_epoch_not_just_last_good_run(self):
        self.write('capture.stdout', self.final)
        self.assertTrue(check.check_closed(self.root)['ok'])
        os.makedirs(os.path.join(self.root, 'capture-crashed', 'journal'))
        self.assertFalse(check.check_closed(self.root)['ok'])

    def test_closed_requires_zero_drops_complete_flush_and_both_channels(self):
        self.write('capture.stdout', self.final)
        self.assertTrue(check.check_closed(self.root)['ok'])
        self.final['stats']['kernel_drops'] = 1
        self.write('capture.stdout', self.final)
        self.assertFalse(check.check_closed(self.root)['ok'])
        self.final['stats']['kernel_drops'] = 0
        self.final['journal_events'] = 29
        self.write('capture.stdout', self.final)
        self.assertFalse(check.check_closed(self.root)['ok'])
        self.final['journal_events'] = 30
        self.final['channels'][1]['datagrams'] = 0
        self.write('capture.stdout', self.final)
        self.assertFalse(check.check_closed(self.root)['ok'])


if __name__ == '__main__':
    unittest.main()

