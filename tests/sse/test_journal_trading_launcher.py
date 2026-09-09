#!/usr/bin/env python2
from __future__ import print_function
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

def native_stub():
    # Stand in for the native binary. Read its inherited anonymous profile FD.
    with open(sys.argv[2]) as source: value = json.load(source)
    print(json.dumps({'value': value, 'order_env': os.environ.get('SSE_ENABLE_LIVE_ORDER'),
                      'profile_link': os.readlink(sys.argv[2]), 'argv': sys.argv[1:]}))


class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.mkdtemp(prefix='sse-trading-launcher-test-')
        self.day = int(time.strftime('%Y%m%d'))
        self.live = {'runtime_root': self.temp, 'account_reference': 'fake-account', 'daily_config_pattern':self.temp+'/config_sse_daily_%Y%m%d.json',
                     'td_library': '/fake/td.so', 'td_config': '/fake/private.json',
                     'fee_reserve_per_order': 0.0, 'trading_enabled': True, 'production_approval': True}
        for key in ('model_path','snapshot_baseline_model_path','snapshot_baseline_scaler_path',
                    'snapshot_auction59_model_path','snapshot_auction59_scaler_path'):
            self.live[key] = '/fake/' + key
        self.daily = {'trading_day': self.day, 'global_params': {'offset': 0.8, 'position_base_line': 100000,
                        'position_limit': 1, 'global_bias_factor': 1},
                      'ins_params': {'600000.SH': {'Date': self.day, 'Close': 10, 'HistoryAmount': 8000000,
                        'FreeShare': 1000000, 'HpUpperPrice': 11, 'HpLowerPrice': 9,
                        'HistoryVolatility20d': 0.01, 'static_position': 200, 'last_position': 999}}}
        self.capture = {'trading_day': self.day}
        self.launcher = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath(
            os.path.join(os.path.dirname(__file__), '../../tools/sse/run_journal_trading.py'))
    def tearDown(self):
        shutil.rmtree(self.temp)
    def run_launcher(self, *extra):
        with open(os.path.join(self.temp, 'live.json'), 'w') as out: json.dump(self.live, out)
        with open(time.strftime(self.live['daily_config_pattern']), 'w') as out: json.dump(self.daily, out)
        with open(os.path.join(self.temp, 'capture.json'), 'w') as out: json.dump(self.capture, out)
        cmd = [sys.executable, self.launcher, '--live-config', self.temp+'/live.json',
               '--capture-config', self.temp+'/capture.json',
               '--binary', os.path.abspath(__file__)] + list(extra)
        env = dict(os.environ, SSE_ENABLE_LIVE_ORDER='YES')
        child = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        out, error = child.communicate()
        return child.returncode, out.decode('utf-8'), error.decode('utf-8')
    def test_monitor_inherits_profile_and_real_positions(self):
        code, out, error = self.run_launcher(); self.assertEqual(0, code, error)
        report = json.loads(out.strip().splitlines()[-1]); p = report['value']
        self.assertEqual('monitor', p['execution']); self.assertEqual('NO', report['order_env'])
        self.assertFalse(p['strategy_runtime']['td']['trading_enabled'])
        self.assertNotIn('last_position', p['strategy_runtime']['legacy_config']['ins_params']['600000.SH'])
        self.assertEqual(200, p['strategy_runtime']['legacy_config']['ins_params']['600000.SH']['static_position'])
        self.assertIn('(deleted)', report['profile_link'])
        self.assertIn('/td-monitor/', p['strategy_runtime']['oms']['journal_path'])
        first = p['strategy_runtime']['td']['epoch']
        code, out, error = self.run_launcher(); self.assertEqual(0, code, error)
        self.assertGreater(json.loads(out.strip().splitlines()[-1])['value']['strategy_runtime']['td']['epoch'], first)
    def test_query_only_requires_no_model_or_capture(self):
        self.live.pop('model_path'); self.daily.pop('global_params'); self.capture['trading_day']=20000101
        code, out, error = self.run_launcher('--query-only'); self.assertEqual(0, code, error)
        report = json.loads(out.strip().splitlines()[-1]); self.assertEqual('--td-query-only', report['argv'][0])
        self.assertEqual('NO', report['order_env']); self.assertIn('epoch', report['value'])
    def test_date_and_live_order_gates(self):
        self.live['production_approval']=False
        code, out, error = self.run_launcher('--live-orders'); self.assertNotEqual(0, code)
        self.daily['trading_day']=20000101
        code, out, error = self.run_launcher(); self.assertNotEqual(0, code)
    def test_numeric_and_duration_checks(self):
        self.daily['global_params']['position_limit']=True
        code, out, error=self.run_launcher(); self.assertNotEqual(0,code)
        self.daily['global_params']['position_limit']=float('nan')
        code, out, error=self.run_launcher(); self.assertNotEqual(0,code)
        self.daily['global_params']['position_limit']=1
        code,out,error=self.run_launcher('--duration-ms','50'); self.assertEqual(0,code,error)
        self.assertEqual(['--duration-ms','50'],json.loads(out.strip().splitlines()[-1])['argv'][-2:])
        code,out,error=self.run_launcher('--live-orders'); self.assertEqual(0,code,error)
        p=json.loads(out.strip().splitlines()[-1])['value']
        self.assertIn('/td/',p['strategy_runtime']['oms']['journal_path'])
    def write_history(self, day, globals_):
        value={'trading_day': day, 'global_params': globals_, 'ins_params': {}}
        with open(self.temp+'/config_sse_daily_%08d.json' % day, 'w') as out: json.dump(value, out)
    def previous_day(self):
        return int(time.strftime('%Y%m%d', time.localtime(time.time()-86400)))
    def test_global_params_current_daily_wins(self):
        previous = {'offset': 9.0, 'position_base_line': 900000,
                    'position_limit': 9, 'global_bias_factor': 9}
        self.write_history(self.previous_day(), previous)
        code, out, error = self.run_launcher()
        self.assertEqual(0, code, error)
        profile = json.loads(out.strip().splitlines()[-1])['value']
        self.assertEqual(0.8, profile['strategy_runtime']['legacy_config']['global_params']['offset'])
    def test_global_params_missing_inherits_without_static_fallback(self):
        previous = {'offset': 1.25, 'position_base_line': 125000,
                    'position_limit': 2, 'global_bias_factor': 0.75}
        previous_day = self.previous_day()
        self.write_history(previous_day, previous)
        self.daily.pop('global_params')
        code, out, error = self.run_launcher()
        self.assertEqual(0, code, error)
        profile = json.loads(out.strip().splitlines()[-1])['value']
        self.assertEqual(previous, profile['strategy_runtime']['legacy_config']['global_params'])
        self.assertEqual(200, profile['strategy_runtime']['legacy_config']['ins_params']['600000.SH']['static_position'])
        self.assertIn('global_params inherited from daily %08d' % previous_day, error)
        with open(time.strftime(self.live['daily_config_pattern'])) as source: current = json.load(source)
        self.assertNotIn('global_params', current)
    def test_global_params_missing_uses_nearest_valid_past_not_future(self):
        older = int(time.strftime('%Y%m%d', time.localtime(time.time()-2*86400)))
        previous = self.previous_day()
        future = int(time.strftime('%Y%m%d', time.localtime(time.time()+86400)))
        self.write_history(older, {'offset': 2.0, 'position_base_line': 200000,
                                   'position_limit': 2, 'global_bias_factor': 2})
        self.write_history(previous, {'offset': 1.0, 'position_base_line': 100000,
                                      'position_limit': 1, 'global_bias_factor': 1})
        self.write_history(future, {'offset': 99.0, 'position_base_line': 990000,
                                    'position_limit': 99, 'global_bias_factor': 99})
        self.daily.pop('global_params')
        code, out, error = self.run_launcher()
        self.assertEqual(0, code, error)
        profile = json.loads(out.strip().splitlines()[-1])['value']
        self.assertEqual(1.0, profile['strategy_runtime']['legacy_config']['global_params']['offset'])
        self.assertIn('global_params inherited from daily %08d' % previous, error)
    def test_global_params_present_but_malformed_rejects_without_fallback(self):
        previous = {'offset': 1.25, 'position_base_line': 125000,
                    'position_limit': 2, 'global_bias_factor': 0.75}
        self.write_history(self.previous_day(), previous)
        self.daily['global_params'] = {'offset': 'invalid', 'position_base_line': 100000,
                                       'position_limit': 1, 'global_bias_factor': 1}
        code, out, error = self.run_launcher()
        self.assertNotEqual(0, code)
        self.assertIn('global_params.offset', error)
    def test_global_params_missing_without_history_rejects(self):
        self.daily.pop('global_params')
        code, out, error = self.run_launcher()
        self.assertNotEqual(0, code)
        self.assertIn('no valid historical daily global_params', error)

if __name__ == '__main__':
    os.environ['TZ']='Asia/Shanghai'; time.tzset()
    if len(sys.argv) in (3,5) and (sys.argv[1]=='--td-query-only' or sys.argv[1].endswith('capture.json')):
        native_stub()
    else:
        unittest.main(argv=[sys.argv[0]])
