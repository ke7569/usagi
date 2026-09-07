"""Guard ownership boundaries without freezing implementation or artifact versions."""
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {'.h', '.hpp', '.cpp', '.cc', '.c'}
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"\n]+)"', re.MULTILINE)


def sources(directory):
    for path in (ROOT / directory).rglob('*'):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


class SourceLayoutTest(unittest.TestCase):
    def test_symmetric_market_responsibilities(self):
        for market in ('sze', 'sse'):
            for part in ('market_data', 'sampling', 'factors', 'model', 'runtime'):
                self.assertTrue((ROOT / market / part).is_dir(), market + '/' + part)

    def test_common_does_not_depend_on_market_or_sdk_adapter(self):
        for path in sources('common'):
            for include in INCLUDE.findall(path.read_text(encoding='utf-8')):
                self.assertFalse(include.startswith(('sze/', 'sse/', 'adapters/')),
                                 '{} includes {}'.format(path.relative_to(ROOT), include))

    def test_market_modules_do_not_include_each_other(self):
        for market, other in (('sze', 'sse/'), ('sse', 'sze/')):
            for path in sources(market):
                for include in INCLUDE.findall(path.read_text(encoding='utf-8')):
                    self.assertFalse(include.startswith(other),
                                     '{} includes {}'.format(path.relative_to(ROOT), include))

    def test_repository_qualified_includes_resolve(self):
        prefixes = ('common/', 'sze/', 'sse/', 'adapters/', 'apps/',
                    'third_party/', 'reference/', 'tests/')
        old_prefixes = ('src/t0-main/', 'sse-t0/', 'modules/deepwin_guoxin/')
        for directory in ('common', 'sze', 'sse', 'adapters', 'apps', 'tests', 'tools'):
            for path in sources(directory):
                for include in INCLUDE.findall(path.read_text(encoding='utf-8')):
                    self.assertFalse(any(prefix in include for prefix in old_prefixes),
                                     '{} has old include {}'.format(path.relative_to(ROOT), include))
                    if include.startswith(prefixes):
                        self.assertTrue((ROOT / include).is_file(),
                                        '{} includes missing {}'.format(path.relative_to(ROOT), include))

    def test_one_configuration_toolchain(self):
        for name in ('unified_config.py', 'prepare_stream_processing.py'):
            self.assertTrue((ROOT / 'tools/config' / name).is_file())
        for market in ('sze', 'sse'):
            self.assertFalse((ROOT / market / 'config').exists())
        for path in ('src/t0-main', 'sse-t0', 'modules/deepwin_guoxin'):
            self.assertFalse((ROOT / path).exists(), path)


if __name__ == '__main__':
    unittest.main()
