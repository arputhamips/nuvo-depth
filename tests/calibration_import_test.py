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
import shutil
import threading
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import patch
from calibration_test_support import prepare_root,bundle,write_json
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import nuvo_calibration_control as control
from nuvo_calibration_store import CalibrationStore
import nuvo_calibration_store as store_module

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
        self.assertIn('recovery is pending',control.readiness(self.root,self.native)['problem'])
        with self.assertRaisesRegex(RuntimeError,'disabled after interrupted'):
            control.recover(self.root,startup=True)
        self.assertFalse((self.root/control.MARKER).exists())
        self.assertFalse((self.source.parent/'active').exists())
        records=[json.loads(p.read_text()) for p in store.transactions.glob('*.json')]
        self.assertEqual(records[-1]['state'],'aborted')
    def test_every_durable_activation_boundary_recovers_the_committed_policy(self):
        points=('journal-staging','bundle-installed','journal-awaiting_restart','active-switched',
                'policy-marker-written','policy-override-written','policy-installed',
                'journal-restoring','journal-committed')
        for point in points:
            with self.subTest(point=point),tempfile.TemporaryDirectory() as temporary:
                root=Path(temporary);prepare_root(root)
                original,value=bundle(root);(original.parent/'active').unlink()
                source=root/'external-input';shutil.copytree(original,source);shutil.rmtree(original)
                marker=root/control.MARKER;marker.parent.mkdir(parents=True,exist_ok=True);marker.write_text('explicit-preview\n')
                override=root/control.POLICY_OVERRIDE;override.parent.mkdir(parents=True,exist_ok=True);override.write_text('[Service]\n# preserved policy sentinel\n')
                policy=control.CapturePolicy(root);before=policy.snapshot()['files']
                store=CalibrationStore(root/control.STATE,policy=policy)
                real_json=store_module.atomic_json;real_replace=store_module.os.replace
                real_active=store._replace_active;real_apply=policy.apply;real_bytes=control.atomic_bytes
                def stopped():raise SystemExit('simulated process loss: '+point)
                def journal(path,record):
                    real_json(path,record)
                    if path.parent==store.transactions and point=='journal-'+record['state']:stopped()
                def replaced(source_path,target):
                    real_replace(source_path,target)
                    if point=='bundle-installed' and Path(target)==original:stopped()
                def active(namespace,identity):
                    real_active(namespace,identity)
                    if point=='active-switched' and identity==value['bundle_id']:stopped()
                def written(path,data):
                    real_bytes(path,data)
                    if (point=='policy-marker-written' and path==marker or
                            point=='policy-override-written' and path==override):stopped()
                def applied(snapshot):
                    real_apply(snapshot)
                    if point=='policy-installed':stopped()
                def restarted(identity):
                    if point=='journal-restoring':raise RuntimeError('force service recovery path')
                with patch.object(store_module,'atomic_json',journal),patch.object(store_module.os,'replace',replaced), \
                     patch.object(store,'_replace_active',active),patch.object(control,'atomic_bytes',written), \
                     patch.object(policy,'apply',applied):
                    with self.assertRaises(SystemExit):store.activate(source,restart=restarted)
                committed=point=='journal-committed'
                self.assertEqual(store.recover_before_start(),not committed)
                active_path=original.parent/'active'
                self.assertEqual(os.readlink(active_path) if active_path.is_symlink() else None,value['bundle_id'] if committed else None)
                expected=policy.accepted({'files':before})['files'] if committed else before
                self.assertEqual(policy.snapshot()['files'],expected)
                self.assertFalse(store.recover_before_start())
                journals=[json.loads(path.read_text()) for path in store.transactions.glob('*.json')]
                self.assertEqual(journals[-1]['state'],'committed' if committed else 'aborted')
    def test_concurrent_import_and_rollback_serialize_without_mixed_selection(self):
        control.import_bundle(self.root,self.source,False,self.native)
        second,value=bundle(self.root,provenance={'synthetic_revision':'concurrent'})
        (second.parent/'active').unlink();(second.parent/'active').symlink_to(self.value['bundle_id'])
        entered=threading.Event();release=threading.Event();rollback_started=threading.Event()
        def paused_native(args,**kwargs):
            if args[:2]==['systemctl','restart']:
                entered.set()
                if not release.wait(5):raise RuntimeError('test did not release live activation')
            return self.native(args,**kwargs)
        def rollback():
            rollback_started.set();return control.rollback(self.root,run=self.native)
        with ThreadPoolExecutor(max_workers=2) as executor:
            importing=executor.submit(control.import_bundle,self.root,second,False,paused_native)
            try:
                self.assertTrue(entered.wait(5))
                restoring=executor.submit(rollback);self.assertTrue(rollback_started.wait(5))
                self.assertFalse(CalibrationStore(self.root/control.STATE,policy=control.CapturePolicy(self.root)).recover_before_start())
                self.assertFalse(restoring.done())
            finally:release.set()
            self.assertEqual(importing.result(timeout=5)['bundle_id'],value['bundle_id'])
            self.assertEqual(restoring.result(timeout=5)['bundle_id'],self.value['bundle_id'])
        self.assertEqual(os.readlink(second.parent/'active'),self.value['bundle_id'])
        self.assertEqual(control.daemon_health(self.root)['calibration_sha256'],self.value['bundle_id'])
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
    def test_full_installer_uses_verified_import_and_restores_on_startup_failure(self):
        import nuvo_setup as setup
        from unittest.mock import patch
        (self.root/'etc/hostname').write_text('test-unit\n')
        plan={'factory':False,'access_policy':'keep_current','display_name':'Test','hardware':'nuvo',
              'startup':True,'engineering':False,'wifi_interface':'wlan0','wifi_country':'US',
              'ap_address':'192.168.4.1/24','calibration_bundle':str(self.source),'calibration_namespace':'user'}
        calls=[]
        def runner(args,**kwargs):
            calls.append(args)
            if args[:3]==['nuvoctl','calibration','import']:
                result=control.import_bundle(self.root,Path(args[3]),'--dry-run' in args,self.native)
                return subprocess.CompletedProcess(args,0,json.dumps(result),'')
            if 'provisioning-status' in args:
                return subprocess.CompletedProcess(args,0,'{"accepted":false}','')
            return subprocess.CompletedProcess(args,0,'','')
        self.fail_restart=True
        with patch.object(setup.admin,'provision_identity',return_value={'device_uuid':'unit-1'}), \
             patch.object(setup.admin,'platform_prepare',return_value={'reboot_required':False}):
            with self.assertRaisesRegex(RuntimeError,'not verified'):
                setup.apply_configuration(self.root,plan,runner)
            self.assertFalse((self.source.parent/'active').exists())
            self.assertFalse((self.root/control.MARKER).exists())
            result=setup.apply_configuration(self.root,plan,runner)
        self.assertTrue((self.root/control.MARKER).exists())
        self.assertEqual(control.readiness(self.root,self.native)['state'],'calibration_accepted')
        self.assertFalse(any(args[0].endswith('/nuvo-calibration') and 'activate' in args for args in calls))
        self.assertGreaterEqual(self.restarts,2)

if __name__=='__main__':unittest.main()
