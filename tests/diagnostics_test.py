"""Stable failures, explicit omissions and credential exclusion for support exports."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import nuvo_diagnostics as diagnostics

def fixture():
    return {'profile':'appliance','model':'Raspberry Pi 5 Model B','codename':'trixie','kernel':'qualified',
            'platform':{'kernels':['qualified']},'camera_count':2,'trigger':'1\n','service':'active',
            'health':{'status':'running','matcher_active':False,'capture_target_hz':10,'matched_frames':0},
            'readiness':{'state':'preview_enabled','capture_enabled':True,'authority':'provisional_recalibration_required'},
            'encoding':{'mode':'raw'},'versions':{'nuvo-depth-core':'0.1.1~beta.8'},
            'approval':{'approved':True,'repository':'arputhamips/nuvo-depth','tag':'v0.1.1-beta.6'}}

class DiagnosticsTest(unittest.TestCase):
    def checks(self,inputs): return {item['id']:item for item in diagnostics.build_report(inputs)['checks']}
    def test_standard_os_release_link_is_read_without_following_other_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/'etc').mkdir(); (root/'usr/lib').mkdir(parents=True)
            target=root/'usr/lib/os-release'; target.write_text('ID=debian\nVERSION_CODENAME=trixie\n')
            link=root/'etc/os-release'; link.symlink_to('../usr/lib/os-release')
            self.assertEqual(diagnostics.os_release(root),target.read_text())
            link.unlink(); link.symlink_to('/etc/os-release')
            self.assertIsNone(diagnostics.os_release(root))
            link.unlink(); link.symlink_to('../usr/lib/os-release')
            target.unlink(); target.symlink_to('/etc/os-release')
            self.assertIsNone(diagnostics.os_release(root))
    def test_failures_have_stable_ids_and_actions(self):
        for key,value,identity in [('camera_count',1,'capture.cameras'),('kernel','unsupported','platform.kernel'),
                                   ('trigger','0','capture.trigger'),('service','failed','capture.service'),
                                   ('codename','bookworm','platform.os')]:
            with self.subTest(identity=identity):
                inputs=fixture(); inputs[key]=value
                check=self.checks(inputs)[identity]
                self.assertEqual(check['status'],'fail'); self.assertTrue(check['next_action'])
        inputs=fixture(); inputs['camera_count']=None
        self.assertEqual(self.checks(inputs)['capture.cameras']['status'],'unknown')
    def test_idle_and_minimal_are_not_false_disconnections(self):
        inputs=fixture(); checks=self.checks(inputs)
        self.assertEqual(checks['output.depth']['status'],'pass')
        self.assertIn('idle',checks['output.depth']['reason'])
        inputs.update(profile='minimal',service='inactive',readiness={'state':'calibration_needed','capture_enabled':False})
        checks=self.checks(inputs)
        self.assertEqual(checks['capture.service']['status'],'warning')
        self.assertIn('intentionally omits',checks['output.monitoring']['reason'])
        self.assertIn('local nuvoctl',checks['output.monitoring']['next_action'])
    def test_export_excludes_every_secret_class_and_free_form_errors(self):
        secrets=['TOKEN-CANARY','PASSWORD-CANARY','COOKIE-CANARY','PRIVATE-KEY-CANARY',
                 'SETUP-SECRET-CANARY','CONFIRMATION-LINK-CANARY']
        inputs=fixture()
        for key in ('health','readiness','encoding','versions','platform','approval'):
            inputs[key]['private_material']=' '.join(secrets)
        inputs['health']['error']=' '.join(secrets)
        inputs['readiness']['problem']=' '.join(secrets)
        inputs['readiness']['next_action']=' '.join(secrets)
        inputs['versions']['nuvo-depth-gateway']=secrets[0]
        inputs['health']['processing_ms']=float('nan')
        output=json.dumps(diagnostics.build_report(inputs),allow_nan=False)
        self.assertTrue(all(secret not in output for secret in secrets))
        self.assertIn('0.1.1~beta.8',output)
        self.assertFalse(json.loads(output)['privacy']['automatic_upload'])
    def test_collection_does_not_write_persistent_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            path=root/'etc/nuvo-depth/stereo.json'; path.parent.mkdir(parents=True); path.write_text('{"private":"sentinel"}')
            before={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file()}
            def runner(args,**kwargs): return subprocess.CompletedProcess(args,1,'','not available')
            report=diagnostics.collect(root,'minimal',runner)
            after={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file()}
            self.assertEqual(before,after)
            self.assertNotIn('sentinel',json.dumps(report))

if __name__=='__main__': unittest.main()
