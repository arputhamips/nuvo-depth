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
        if args[:2]==['systemctl','is-active']:
            value=getattr(self,'service_state','active')
            return subprocess.CompletedProcess(args,0 if value=='active' else 3,value+'\n','')
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
    def test_malformed_health_and_transaction_shapes_stay_unavailable(self):
        for value in (None,'invalid',[],True,99):
            with self.subTest(authority=value):
                write_json(self.root/'run/nuvo-depth/status.json',{'status':'running','calibration_authority':value})
                ready=self.status()
                self.assertEqual(ready['state'],'unavailable')
                self.assertEqual(ready['reason'],'daemon_status_malformed')
        (self.root/'run/nuvo-depth/status.json').unlink()
        for value in (None,[],1,{'state':[]}, {'state':'staging','namespace':[]}):
            with self.subTest(transaction=value):
                write_json(self.root/control.STATE/'transactions/calibration-invalid.json',value)
                ready=self.status()
                self.assertEqual(ready['state'],'unavailable')
                self.assertIn('transaction',ready['problem'].lower())
    def test_accepted_bindings_and_loaded_observation_remain_distinct(self):
        target,value=bundle(self.root)
        ready=self.status()
        self.assertEqual(ready['bindings']['unit_id'],'unit-1')
        self.assertEqual(ready['authority'],'user_validated')
        self.assertEqual(ready['calibration_sha256'],value['bundle_id'])
        self.assertIsNone(ready['loaded'])
        self.assertEqual(ready['runtime_verification']['state'],'unverified')
        self.assertFalse(ready['runtime_verification']['fresh_frames_verified'])
        (self.root/control.MARKER).write_text('enabled\n')
        health={'status':'running','run_uuid':'current-run','calibration_sha256':value['bundle_id'],'calibration_authority':1}
        write_json(self.root/'run/nuvo-depth/status.json',health)
        receipt={'schema_version':1,'bundle_id':value['bundle_id'],'authority':1,'run_uuid':'current-run','capture_sequences':[10,11],'verified_at_unix_ns':123}
        write_json(self.root/control.STATE/'calibration-verification.json',receipt)
        self.assertTrue(self.status()['runtime_verification']['fresh_frames_verified'])
        for key,replacement in (('run_uuid','old-run'),('authority',2),('bundle_id','f'*64)):
            write_json(self.root/control.STATE/'calibration-verification.json',{**receipt,key:replacement})
            self.assertFalse(self.status()['runtime_verification']['fresh_frames_verified'])
        write_json(self.root/control.STATE/'calibration-verification.json',receipt)
        (self.root/'run/systemd/system').mkdir(parents=True)
        self.service_state='inactive'
        stopped=self.status()
        self.assertTrue(stopped['accepted'])  # Bundle validation remains separate.
        self.assertIsNone(stopped['loaded'])
        self.assertFalse(stopped['runtime_verification']['fresh_frames_verified'])
        self.assertEqual(stopped['camera_service']['error_code'],'daemon_service_not_running')

if __name__=='__main__':unittest.main()
