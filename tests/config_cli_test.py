"""Compare the lightweight configuration CLI with the existing daemon CLI."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CONFIG_TOOL, DAEMON = map(Path, sys.argv[1:3])
del sys.argv[1:3]


class ConfigCliTest(unittest.TestCase):
    def assert_parity(self, command, path, expected):
        results = [subprocess.run([str(binary), command, str(path)],
                                  capture_output=True, timeout=10)
                   for binary in (CONFIG_TOOL, DAEMON)]
        for result in results:
            self.assertEqual(result.returncode, expected, result.stderr.decode())
        self.assertEqual(results[0].stdout, results[1].stdout)
        self.assertEqual(results[0].stderr.replace(b'depthd-config:', b'depthd:'),
                         results[1].stderr)
        return results[0]

    def test_valid_profiles_and_warnings_are_byte_identical(self):
        profiles = [ROOT / 'config/nuvo_pi5_depth_100mm.yaml',
                    ROOT / 'tests/fixtures/config/valid-schema-v1.yaml']
        for path in profiles:
            for command in ('--dump-effective-config', '--check-config'):
                with self.subTest(path=path, command=command):
                    result = self.assert_parity(command, path, 0)
                    self.assertTrue(result.stdout)
        result = self.assert_parity('--check-config', profiles[0], 0)
        self.assertIn(b'warning: rig.calibration_status:', result.stdout)

    def test_invalid_yaml_schema_values_and_missing_file_keep_errors_and_exit_codes(self):
        valid = (ROOT / 'config/nuvo_pi5_depth_100mm.yaml').read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'daemon.yaml'
            for content in ('[unterminated', 'unknown: field\n',
                            valid.replace('schema_version: 1', 'schema_version: 999'),
                            valid.replace('baseline_m: 0.0996937500869897', 'baseline_m: -1')):
                path.write_text(content)
                for command in ('--dump-effective-config', '--check-config'):
                    with self.subTest(content=content[:40], command=command):
                        result = self.assert_parity(command, path, 2)
                        self.assertEqual(result.stdout, b'')
                        self.assertIn(b'configuration validation failed:', result.stderr)
            path.unlink()
            for command in ('--dump-effective-config', '--check-config'):
                result = self.assert_parity(command, path, 3)
                self.assertEqual(result.stdout, b'')
                self.assertIn(b'configuration I/O error:', result.stderr)

    def test_usage_errors_do_not_read_configuration(self):
        for arguments in ([], ['--check-config'], ['--dump-effective-config', 'a', 'b'],
                          ['--unknown', 'nonexistent']):
            result = subprocess.run([str(CONFIG_TOOL), *arguments], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, b'')
            self.assertIn(b'Usage:', result.stderr)
            self.assertNotIn(b'configuration I/O error', result.stderr)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'ELF dependency check requires Linux')
    def test_transitive_linkage_contains_only_yaml_and_cxx_runtime(self):
        linked = subprocess.check_output(['ldd', str(CONFIG_TOOL)], text=True)
        self.assertNotIn('not found', linked)
        allowed = re.compile(r'^(libyaml-cpp|libstdc\+\+|libgcc_s|libc|libm|libpthread|librt|libdl)\.so(?:\.|$)|^ld-linux|^linux-vdso')
        libraries = [Path(line.split()[0]).name for line in linked.splitlines() if line.strip()]
        self.assertTrue(libraries)
        for library in libraries:
            self.assertRegex(library, allowed, linked)


if __name__ == '__main__':
    unittest.main()
