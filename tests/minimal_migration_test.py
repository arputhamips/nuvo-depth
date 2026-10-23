"""Upgrade the old effective policy before its unit file is replaced; no daemon needed."""
from pathlib import Path
import builtins
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
scope = {'__name__':'migration_fixture'}
source = (ROOT/'debian/nuvo-depth-minimal.preinst').read_text().split("<<'PY'\n",1)[1].split('\nPY\n',1)[0]
exec(compile(source, 'minimal.preinst', 'exec'), scope)

class MigrationTest(unittest.TestCase):
    def fixture(self, guarded=False, preview=True, marker=False):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        service = root/'usr/lib/systemd/system/nuvo-depthd-native.service'
        service.parent.mkdir(parents=True)
        service.write_text('[Unit]\n'+('ConditionPathExists=/etc/nuvo-depth/capture-enabled\n' if guarded else '')+
                           '[Service]\nExecStart=/usr/bin/depthd --run /etc/nuvo-depth/daemon.yaml'+
                           (' --allow-provisional-depth' if preview else '')+'\n')
        if marker:
            path = root/'etc/nuvo-depth/capture-enabled'; path.parent.mkdir(parents=True); path.write_text('existing\n')
        return root
    def test_old_implicit_preview_survives_without_runtime_status(self):
        root = self.fixture()
        scope['migrate'](root)
        self.assertTrue((root/'etc/nuvo-depth/capture-enabled').is_file())
        override = root/'etc/systemd/system/nuvo-depthd-native.service.d/90-calibration-policy.conf'
        self.assertIn('--allow-provisional-depth',override.read_text())
        before = override.read_bytes(); scope['migrate'](root)
        self.assertEqual(override.read_bytes(),before)
    def test_guarded_explicitly_disabled_capture_stays_disabled(self):
        root = self.fixture(guarded=True)
        scope['migrate'](root)
        self.assertFalse((root/'etc/nuvo-depth/capture-enabled').exists())
        self.assertFalse((root/'etc/systemd/system/nuvo-depthd-native.service.d').exists())
        # Exercise postinst too: the historical fallback incorrectly re-enabled
        # a deliberately disabled unit from a leftover validated health file.
        identity=root/'var/lib/nuvo-depth/identity/device.uuid'
        identity.parent.mkdir(parents=True);identity.write_text('preserved-unit\n')
        status=root/'run/nuvo-depth/status.json';status.parent.mkdir(parents=True)
        status.write_text('{"status":"running","calibration_authority":1}')
        real_import=builtins.__import__
        def importing(name,*args,**kwargs):
            if name=='pathlib':return SimpleNamespace(Path=lambda value:root/value.lstrip('/'))
            return real_import(name,*args,**kwargs)
        post=(ROOT/'debian/nuvo-depth-minimal.postinst').read_text().split("<<'PY'\n",1)[1].split('\nPY\n',1)[0]
        exec(compile(post,'minimal.postinst','exec'),{'__builtins__':dict(vars(builtins),__import__=importing)})
        self.assertFalse((root/'etc/nuvo-depth/capture-enabled').exists())
        self.assertEqual(identity.read_text(),'preserved-unit\n')
    def test_guarded_preview_and_validated_policy_keep_the_original_command(self):
        for preview in (True,False):
            with self.subTest(preview=preview):
                root = self.fixture(guarded=True,preview=preview,marker=True)
                scope['migrate'](root)
                text = (root/'etc/systemd/system/nuvo-depthd-native.service.d/90-calibration-policy.conf').read_text()
                self.assertEqual('--allow-provisional-depth' in text,preview)
                self.assertEqual((root/'etc/nuvo-depth/capture-enabled').read_text(),'existing\n')
    def test_legacy_loose_calibration_override_is_preserved(self):
        root = self.fixture()
        override = root/'etc/systemd/system/nuvo-depthd-native.service.d/50-legacy.conf'
        override.parent.mkdir(parents=True)
        override.write_text('[Service]\nExecStart=\nExecStart=/usr/bin/depthd --run /etc/nuvo-depth/daemon.yaml --calibration-root /etc/nuvo-depth/calibration\n')
        scope['migrate'](root)
        text = override.with_name('90-calibration-policy.conf').read_text()
        self.assertIn('--calibration-root /etc/nuvo-depth/calibration',text)
        self.assertNotIn('--allow-provisional-depth',text)

if __name__ == '__main__': unittest.main()
