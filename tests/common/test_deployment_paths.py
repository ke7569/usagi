"""Relocated packaging/launcher checks using a stub, never market connectivity."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class DeploymentPathsTest(unittest.TestCase):
    def run_script(self, path, args, env, cwd):
        return subprocess.run(['bash', str(path)] + args, env=env, cwd=str(cwd),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)

    def test_source_and_packaged_capture_launcher_resolve_the_binary(self):
        with tempfile.TemporaryDirectory(prefix='usagi-package-paths-') as temporary:
            directory = Path(temporary)
            build = directory / 'fixture-build'
            build.mkdir()
            binary = build / 'sse_udp_observer'
            binary.write_text('#!/bin/sh\necho USAGI_TEST_STUB\nexit 19\n', encoding='ascii')
            binary.chmod(0o755)
            env = os.environ.copy()
            env.pop('SSE_CAPTURE_BINARY', None)
            env.update(USAGI_BUILD_DIR=str(build), USAGI_PACKAGE_DIR=str(directory / 'packages'),
                       SSE_PACKAGE_STAMP='fixture')
            packaged = self.run_script(ROOT / 'deploy/sse/build_live_package.sh', [], env, directory)
            self.assertEqual(packaged.returncode, 0, packaged.stderr.decode('utf-8', 'replace'))
            package = directory / 'packages/sse-live-capture-fixture'
            self.assertTrue((package / 'README.md').is_file())
            checksums = subprocess.run(['sha256sum', '-c', 'SHA256SUMS'], cwd=str(package),
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertEqual(checksums.returncode, 0, checksums.stderr.decode('utf-8', 'replace'))
            for label, launcher in (
                    ('source', ROOT / 'deploy/sse/start_sse_capture.sh'),
                    ('package', package / 'start_sse_capture.sh')):
                output = directory / label
                started = self.run_script(launcher, ['--site', 'dongguan', '--interface-ip',
                                          '127.0.0.1', '--output-dir', str(output)], env, directory)
                self.assertEqual(started.returncode, 5, started.stderr.decode('utf-8', 'replace'))
                logs = list(output.glob('*.stderr.log'))
                self.assertEqual(len(logs), 1)
                self.assertIn('USAGI_TEST_STUB', logs[0].read_text(encoding='utf-8'))
                self.assertFalse((output / 'sse_capture.pid').exists())

    def test_daily_preparation_requires_external_validator_before_writes(self):
        with tempfile.TemporaryDirectory(prefix='usagi-daily-preflight-') as temporary:
            session = Path(temporary) / 'session'
            env = os.environ.copy()
            env.pop('SSE_STATIC_METADATA_VALIDATOR', None)
            result = self.run_script(ROOT / 'deploy/sse/prepare_sse_daily_runtime.sh',
                                     ['20260904', str(session)], env, Path(temporary))
            self.assertEqual(result.returncode, 5)
            self.assertIn(b'SSE_STATIC_METADATA_VALIDATOR', result.stderr)
            self.assertFalse(session.exists())

    def test_legacy_runtime_package_requires_explicit_artifacts(self):
        with tempfile.TemporaryDirectory(prefix='usagi-runtime-preflight-') as temporary:
            output = Path(temporary) / 'package'
            env = os.environ.copy()
            for key in ('SSE_STRATEGY', 'SSE_TD_LIB', 'SSE_MAIN_BIN',
                        'SSE_MODEL_ARCHIVE', 'SSE_DEPS_ARCHIVE'):
                env.pop(key, None)
            result = self.run_script(ROOT / 'deploy/sse/build_sse_live_runtime_package.sh',
                                     ['20260904', str(output)], env, Path(temporary))
            self.assertEqual(result.returncode, 2)
            self.assertIn(b'SSE_STRATEGY', result.stderr)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
