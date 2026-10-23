import importlib.util
from pathlib import Path
import unittest
from contextlib import redirect_stdout
import io
import tempfile
from unittest import mock
spec=importlib.util.spec_from_file_location('routing',Path(__file__).resolve().parents[1]/'tools/ci/required_checks.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
class RoutingTest(unittest.TestCase):
    def test_hosted_ubuntu_mirror_list_indirection_is_rewritten(self):
        script=(Path(__file__).resolve().parents[1]/'tools/ci/use-ubuntu-mirror.sh').read_text()
        body=script.split("<<'PY'\n",1)[1].rsplit('\nPY',1)[0]
        names=('etc/apt/sources.list','etc/apt/sources.list.d/ubuntu.sources',
               'etc/apt/apt-mirrors.txt','etc/apt/apt-mirrors-security.txt')
        original='http://azure.archive.ubuntu.com/ubuntu priority:1\nhttps://azure.archive.ubuntu.com/ubuntu priority:2\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n'
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for name in names:
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(original)
            with mock.patch('pathlib.Path',lambda name:root/name.lstrip('/')),redirect_stdout(io.StringIO()):
                exec(compile(body,'hosted-mirror-fixture','exec'),{})
            for name in names:
                text=(root/name).read_text()
                self.assertNotIn('azure.archive.ubuntu.com',text)
                self.assertIn('https://archive.ubuntu.com/ubuntu',text)
                self.assertIn('Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg',text)
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
