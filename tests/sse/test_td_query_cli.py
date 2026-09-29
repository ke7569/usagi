#!/usr/bin/env python
"""Query-only CLI rejects invalid input without loading or connecting any TD."""
from __future__ import print_function
import json
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    binary = os.path.abspath(sys.argv[1])
    root = tempfile.mkdtemp(prefix='sse-td-query-cli-test-')
    try:
        process = subprocess.Popen([binary, '--td-query-only'],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, stderr = process.communicate()
        assert process.returncode == 2 and b'PUBLIC_CONFIG' in stderr
        base = {'library': '/no-such-td-plugin.so', 'config_path': '/no-private-config.json',
                'broker': 'fixture', 'account': 'testaccount', 'trading_day': 20260909,
                'daily_config': '/no-daily.json', 'journal_path': '/no-journal.jsonl'}
        cases = [({}, 'requires library'),
                 (dict(base, library='relative.so'), 'absolute library'),
                 (dict(base, timeout_ms=0), 'timeout_ms'),
                 (dict(base, timeout_ms=60001), 'timeout_ms'),
                 (dict(base, timeout_ms='20000'), 'timeout_ms'),
                 (dict(base, trading_day='20260909'), 'trading_day'),
                 (dict(base, trading_day=0), 'trading_day'),
                 (dict(base, epoch=0), 'epoch'),
                 (dict(base, epoch=-1), 'epoch'),
                 (dict(base, epoch=True), 'epoch'),
                 (dict(base, epoch='17'), 'epoch'),
                 (dict(base, epoch=1.5), 'epoch'),
                 (dict(base, epoch=18446744073709551616), 'epoch')]
        for index, (config, expected) in enumerate(cases):
            path = os.path.join(root, '%s.json' % index)
            with open(path, 'w') as output:
                json.dump(config, output)
            process = subprocess.Popen([binary, '--td-query-only', path],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            stdout, stderr = process.communicate()
            assert process.returncode == 1, (stdout, stderr)
            result = json.loads(stdout.decode('utf-8'))
            assert result['event'] == 'td_query_only'
            assert not result['orders_enabled'] and not result['ready'] and not result['connected']
            assert expected in result['reason'], result
            assert 'plugin initialization' not in result['reason'], result
        print('sse_td_query_cli_test: PASS (argument and public-config validation; no TD connection)')
    finally:
        shutil.rmtree(root)


if __name__ == '__main__':
    main()
