"""Synthetic control/transaction fixtures; no accuracy claim or camera access."""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from calibration_test_support import prepare_root,bundle,write_json
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import nuvo_calibration_control as control
from nuvo_calibration_store import CalibrationStore

def frames(value,run_uuid='abcd',authority=1):
    result=b''
    for seq in [3,4]:
        metadata=json.dumps({'schema_version':1,'capture_sequence':seq,'run_uuid':run_uuid,
                             'calibration_sha256':value,'calibration_authority':authority,'depth_unit':'metres',
                             'products':[{'kind':'depth'},{'kind':'validity'}]}).encode()
        result+=struct.pack('>4sHHHHIQQ',b'NUV1',1,0,5,0,len(metadata),0,seq)+metadata
    return result

class ImportTest(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup);self.root=Path(temp.name)
        self.cfg=prepare_root(self.root)
        self.source,self.value=bundle(self.root)
        (self.source.parent/'active').unlink()
        self.restarts=0;self.fail_restart=False;self.wrong_frame=False
    def native(self,args,**kwargs):
        if '--dump-effective-config' in args:return subprocess.CompletedProcess(args,0,json.dumps({'configuration':self.cfg}),'')
        if '--calibration-check' in args:return subprocess.CompletedProcess(args,0,'{"valid":true}','')
        if args[0]=='systemctl':
            if args[1]=='restart':
                self.restarts+=1
                if self.fail_restart:
                    self.fail_restart=False
                    return subprocess.CompletedProcess(args,1,'','injected restart failure')
                selected=control.selected_calibration(self.root,self.native)
                write_json(self.root/'run/nuvo-depth/status.json',{'status':'running','run_uuid':f'abcd{self.restarts}',
                    'calibration_sha256':selected['bundle_id'] if selected else 'e'*64,'calibration_authority':1 if selected else 0})
            return subprocess.CompletedProcess(args,0,'','')
        if str(args[0]).endswith('nuvo-depth-read'):
            health=control.daemon_health(self.root)
            value='f'*64 if self.wrong_frame else health['calibration_sha256']
            self.wrong_frame=False
            return subprocess.CompletedProcess(args,0,frames(value,health['run_uuid'],health['calibration_authority']),b'')
        raise AssertionError(args)
    def test_dry_run_then_import_then_rollback_to_disabled(self):
        result=control.import_bundle(self.root,self.source,True,self.native)
        self.assertTrue(result['dry_run']);self.assertEqual(self.restarts,0)
        self.assertFalse((self.root/control.MARKER).exists())
        result=control.import_bundle(self.root,self.source,False,self.native)
        self.assertEqual(result['verified_capture_sequences'],[3,4])
        self.assertEqual(result['readiness']['state'],'calibration_accepted')
        control.rollback(self.root,run=self.native)
        self.assertFalse((self.root/control.MARKER).exists())
        self.assertFalse((self.source.parent/'active').exists())
    def test_preserves_existing_bundle_and_policy_on_restart_failure(self):
        control.import_bundle(self.root,self.source,False,self.native)
        second,value=bundle(self.root,provenance={'synthetic_revision':2})
        (second.parent/'active').unlink();(second.parent/'active').symlink_to(self.value['bundle_id'])
        before=control.CapturePolicy(self.root).snapshot()['files']
        self.fail_restart=True
        with self.assertRaisesRegex(RuntimeError,'previous selection and service restored'):
            control.import_bundle(self.root,second,False,self.native)
        self.assertEqual(os.readlink(second.parent/'active'),self.value['bundle_id'])
        self.assertEqual(control.CapturePolicy(self.root).snapshot()['files'],before)
        self.assertEqual(control.daemon_health(self.root)['calibration_sha256'],self.value['bundle_id'])
    def test_wrong_fresh_frame_aborts_and_restores_disabled_capture(self):
        self.wrong_frame=True
        with self.assertRaisesRegex(RuntimeError,'Fresh depth metadata'):
            control.import_bundle(self.root,self.source,False,self.native)
        self.assertFalse((self.root/control.MARKER).exists())
        self.assertFalse((self.source.parent/'active').exists())
    def test_interrupted_activation_restores_policy_before_start(self):
        policy=control.CapturePolicy(self.root)
        store=CalibrationStore(self.root/control.STATE,policy=policy)
        def killed(_):raise SystemExit('injected interruption')
        with self.assertRaises(SystemExit):store.activate(self.source,restart=killed)
        self.assertTrue((self.root/control.MARKER).exists())
        with self.assertRaisesRegex(RuntimeError,'disabled after interrupted'):
            control.recover(self.root,startup=True)
        self.assertFalse((self.root/control.MARKER).exists())
        self.assertFalse((self.source.parent/'active').exists())
        records=[json.loads(p.read_text()) for p in store.transactions.glob('*.json')]
        self.assertEqual(records[-1]['state'],'aborted')
    def test_corruption_identity_camera_and_profile_rejected_before_mutation(self):
        for changes in [{'unit_id':'wrong'},{'left_camera_id':'wrong'},{'preset_id':'wrong'},
                        {'profile':{'sensor_mode':'bad','image_width':1,'image_height':1,'frame_rates_hz':[10]}}]:
            target,value=bundle(self.root,**changes)
            (target.parent/'active').unlink()
            with self.assertRaises(ValueError):control.import_bundle(self.root,target,False,self.native)
        (self.source/'front_left.yaml').write_text('corrupt')
        with self.assertRaisesRegex(ValueError,'hash mismatch'):control.import_bundle(self.root,self.source,False,self.native)
        self.assertEqual(self.restarts,0)
    def test_linked_asset_and_duplicate_manifest_are_rejected(self):
        path=self.source/'validation.json';data=path.read_bytes();path.unlink()
        (self.root/'outside.json').write_bytes(data);path.symlink_to(self.root/'outside.json')
        with self.assertRaisesRegex(ValueError,'linked'):control.import_bundle(self.root,self.source,True,self.native)
        path.unlink();path.write_bytes(data)
        manifest=self.source/'manifest.json'
        manifest.write_text(manifest.read_text().replace('"schema_version":1','"schema_version":1,"schema_version":1'))
        with self.assertRaisesRegex(ValueError,'duplicate'):control.import_bundle(self.root,self.source,True,self.native)
    def test_rollback_between_accepted_bundles_and_reinstall_state(self):
        control.import_bundle(self.root,self.source,False,self.native)
        second,value=bundle(self.root,provenance={'synthetic_revision':2})
        (second.parent/'active').unlink();(second.parent/'active').symlink_to(self.value['bundle_id'])
        control.import_bundle(self.root,second,False,self.native)
        write_json(self.root/'var/lib/nuvo-depth-setup/state.json',{'calibration_state':'pending','engineering':True})
        self.assertEqual(control.readiness(self.root,self.native)['bundle_id'],value['bundle_id'])
        result=control.rollback(self.root,run=self.native)
        self.assertEqual(result['bundle_id'],self.value['bundle_id'])
    def test_minimal_shared_modules_have_no_solver_dependencies(self):
        script="import sys;sys.path.insert(0,'tools');import nuvo_calibration_control,nuvo_minimal_cli;assert not any(n in sys.modules for n in ['numpy','cv2','yaml','nuvo_calibration'])"
        subprocess.run([sys.executable,'-S','-c',script],cwd=ROOT,check=True)
    def test_explicit_preview_selection_and_disable(self):
        ready=control.preview(self.root,True,self.native)
        self.assertEqual(ready['state'],'preview_enabled')
        self.assertEqual(control.daemon_health(self.root)['calibration_authority'],0)
        ready=control.preview(self.root,False,self.native)
        self.assertEqual(ready['state'],'calibration_needed')
        self.assertFalse((self.root/control.MARKER).exists())

if __name__=='__main__':unittest.main()
