import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from calibration_test_support import prepare_root,bundle,write_json
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import nuvo_calibration_control as control
import nuvo_setup as setup

class ReadinessTest(unittest.TestCase):
    def setUp(self):
        temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup);self.root=Path(temporary.name)
        self.cfg=prepare_root(self.root)
    def run_native(self,args,**kwargs):
        if '--dump-effective-config' in args:return subprocess.CompletedProcess(args,0,json.dumps({'configuration':self.cfg}),'')
        if '--calibration-check' in args:return subprocess.CompletedProcess(args,0,'{"valid":true}','')
        raise AssertionError(args)
    def status(self):return control.readiness(self.root,self.run_native)
    def test_needed_preview_and_saved_installer_state_drift(self):
        write_json(self.root/'var/lib/nuvo-depth-setup/state.json',{'schema_version':1,'engineering':True,'calibration_state':'accepted'})
        self.assertEqual(self.status()['state'],'calibration_needed')
        (self.root/control.MARKER).write_text('engineering-preview\n')
        self.assertEqual(self.status()['state'],'preview_enabled')
        expected=self.status()
        with patch.object(control,'readiness',return_value=expected):
            displayed=setup.status(self.root)
        self.assertEqual(displayed['calibration_readiness'],expected)
        self.assertEqual(displayed['calibration_state'],'provisional')
    def test_accepted_does_not_depend_on_a_connected_camera(self):
        target,value=bundle(self.root)
        ready=self.status();self.assertEqual(ready['state'],'calibration_accepted');self.assertTrue(ready['accepted'])
        self.assertIn('disabled by policy',ready['message'])
        self.assertEqual(ready['camera_service']['status'],'unavailable')
        write_json(self.root/'run/nuvo-depth/status.json',{'status':'running','run_uuid':'new-run','calibration_sha256':value['bundle_id'],'calibration_authority':1})
        self.assertTrue(self.status()['loaded'])
        write_json(self.root/'run/nuvo-depth/status.json',{'status':'running','run_uuid':'new-run','calibration_sha256':'f'*64,'calibration_authority':1})
        self.assertEqual(self.status()['state'],'unavailable')
        path=self.root/'run/nuvo-depth/status.json';os.utime(path,(1,1))
        self.assertEqual(self.status()['state'],'calibration_accepted')
    def test_incompatible_corrupt_and_unavailable_are_actionable(self):
        target,value=bundle(self.root,unit_id='another-unit')
        self.assertIn('another device',self.status()['problem'])
        (self.root/'var/lib/nuvo-depth/identity/device.uuid').write_text('another-unit')
        (target/'validation.json').write_text('{"status":"fail"}')
        self.assertIn('hash mismatch',self.status()['problem'])
        (self.root/'usr/lib/systemd/system/nuvo-depthd-native.service').unlink()
        self.assertIn('not installed',self.status()['problem'])
    def test_legacy_validated_files_are_preserved_without_inventing_binding(self):
        write_json(self.root/'run/nuvo-depth/status.json',{'status':'running','calibration_sha256':'a'*64,'calibration_authority':1})
        ready=self.status();self.assertEqual(ready['state'],'unavailable')
        self.assertIn('preserved',ready['message']);self.assertIn('does not require recalibration',ready['message'])
    def test_profile_or_native_geometry_failure_is_not_accepted(self):
        bundle(self.root,preset_id='incompatible')
        self.assertIn('capture profile',self.status()['problem'])

if __name__=='__main__':unittest.main()
