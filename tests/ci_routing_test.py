import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('routing',Path(__file__).resolve().parents[1]/'tools/ci/required_checks.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
class RoutingTest(unittest.TestCase):
    def test_documentation_stays_light(self):
        self.assertEqual(module.classify(['README.md','docs/install.md','deployment/public-release.json']),{'native':False,'browser':False,'docs_only':True})
    def test_native_sdk_ui_and_calibration_tests_require_runtime_coverage(self):
        for path in ('src/contract.cpp','CMakeLists.txt','include/nuvo/depth.h','sdk/python/nuvo_depth/client.py',
                     'web/app.js','tests/calibration_readiness_test.py','debian/nuvo-depth-minimal.preinst'):
            with self.subTest(path=path): self.assertTrue(module.classify([path])['native'])
    def test_unknown_or_mixed_changes_fail_safe(self):
        self.assertTrue(module.classify([])['browser'])
        self.assertTrue(module.classify(['docs/install.md','unknown-file'])['native'])
        self.assertTrue(module.classify(['docs/example.py'])['native'])
        self.assertTrue(module.classify(['docs/check.sh'])['browser'])
if __name__=='__main__': unittest.main()
