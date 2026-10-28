"""Stable failures, explicit omissions and credential exclusion for support exports."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import nuvo_diagnostics as diagnostics

MMC_ZERO = '''# Command Timeout Occurred:\t 0
# Command CRC Errors Occurred:\t 0
# Data Timeout Occurred:\t 0
# Data CRC Errors Occurred:\t 0
# Auto-Cmd Error Occurred:\t 0
# ADMA Error Occurred:\t 0
# Tuning Error Occurred:\t 0
# CMDQ RED Errors:\t 0
# CMDQ GCE Errors:\t 0
# CMDQ ICCE Errors:\t 0
# Request Timedout:\t 0
# CMDQ Request Timedout:\t 0
# ICE Config Errors:\t 0
# Controller Timedout errors:\t 0
# Unexpected IRQ errors:\t 0
'''

def fixture():
    return {'profile':'appliance','model':'Raspberry Pi 5 Model B','codename':'trixie','kernel':'qualified',
            'platform':{'kernels':['qualified']},'camera_count':2,'trigger':'1\n','service':'active',
            'health':{'status':'running','matcher_active':False,'capture_target_hz':10,'matched_frames':0},
            'readiness':{'state':'preview_enabled','capture_enabled':True,'authority':'provisional_recalibration_required'},
            'encoding':{'mode':'raw'},'versions':{'nuvo-depth-core':'0.1.1~beta.8'},
            'approval':{'approved':True,'repository':'arputhamips/nuvo-depth','tag':'v0.1.1-beta.6'}}

class DiagnosticsTest(unittest.TestCase):
    def checks(self,inputs): return {item['id']:item for item in diagnostics.build_report(inputs)['checks']}
    def test_mmc_parser_accepts_only_complete_bounded_kernel_counters(self):
        counts=diagnostics.parse_mmc_errors(MMC_ZERO)
        self.assertEqual(len(counts),15)
        self.assertTrue(all(value==0 for value in counts.values()))
        recorded=MMC_ZERO.replace('Command Timeout Occurred:\t 0','Command Timeout Occurred:\t 3')
        self.assertEqual(diagnostics.parse_mmc_errors(recorded)['command_timeout'],3)
        maximum=MMC_ZERO.replace('Command Timeout Occurred:\t 0',
                                f'Command Timeout Occurred:\t {diagnostics.MMC_ERROR_MAX}')
        self.assertEqual(diagnostics.parse_mmc_errors(maximum)['command_timeout'],diagnostics.MMC_ERROR_MAX)
        for invalid in (None,'',MMC_ZERO.split('\n',1)[1],MMC_ZERO+MMC_ZERO,
                        MMC_ZERO+'# Device serial: PRIVATE-CANARY\n',MMC_ZERO+'x'*4097,
                        *(MMC_ZERO.replace('Command Timeout Occurred:\t 0',f'Command Timeout Occurred:\t {value}')
                          for value in ('-1','1.5','true','4294967296','999999999999999999999','0 PRIVATE-CANARY'))):
            with self.subTest(invalid=str(invalid)[:80]):
                self.assertIsNone(diagnostics.parse_mmc_errors(invalid))
    def test_mmc_reader_is_optional_bounded_and_read_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'err_stats'
            self.assertIsNone(diagnostics.read_mmc_errors(path))
            path.write_text(MMC_ZERO,encoding='ascii')
            self.assertEqual(diagnostics.read_mmc_errors(path),diagnostics.parse_mmc_errors(MMC_ZERO))
            self.assertEqual(path.read_text(),MMC_ZERO)
            with mock.patch.object(Path,'open',side_effect=PermissionError):
                self.assertIsNone(diagnostics.read_mmc_errors(path))
            stream=mock.mock_open(read_data=b'x'*(diagnostics.MMC_STATS_LIMIT+1))
            with mock.patch.object(Path,'open',stream):
                self.assertIsNone(diagnostics.read_mmc_errors(path))
            stream().read.assert_called_once_with(diagnostics.MMC_STATS_LIMIT+1)
            path.write_bytes(b'\xff')
            self.assertIsNone(diagnostics.read_mmc_errors(path))
            target=Path(temporary)/'other_stats'; target.write_text(MMC_ZERO,encoding='ascii')
            path.unlink(); path.symlink_to(target)
            self.assertIsNone(diagnostics.read_mmc_errors(path))
    def test_storage_results_do_not_claim_health_or_hide_recovered_errors(self):
        for profile in ('appliance','minimal'):
            inputs=fixture(); inputs['profile']=profile
            self.assertEqual(self.checks(inputs)['platform.storage']['status'],'unknown')
            inputs['mmc_errors']=diagnostics.parse_mmc_errors(MMC_ZERO)
            check=self.checks(inputs)['platform.storage']
            self.assertEqual(check['status'],'pass')
            self.assertIn('No MMC controller errors are recorded',check['reason'])
            self.assertIn('health is unverified',check['reason'])
            inputs['mmc_errors']['command_timeout']=4
            check=self.checks(inputs)['platform.storage']
            self.assertEqual(check['status'],'warning')
            self.assertIn('recovered errors may be absent',check['reason'])
            self.assertIn('SD card and power',check['next_action'])
            exported=diagnostics.build_report(inputs)['storage']
            self.assertEqual(exported['mmc_error_counts']['command_timeout'],4)
            self.assertEqual(exported['counter_scope'],'since_boot_or_reset')
    def test_storage_export_revalidates_the_allowlist(self):
        zero=diagnostics.parse_mmc_errors(MMC_ZERO)
        for invalid in ({},dict(zero,private_material='PRIVATE-CANARY'),
                        *(dict(zero,command_timeout=value) for value in
                          (True,-1,1.5,diagnostics.MMC_ERROR_MAX+1,'PRIVATE-CANARY'))):
            inputs=fixture(); inputs['mmc_errors']=invalid
            report=diagnostics.build_report(inputs)
            self.assertEqual({item['id']:item['status'] for item in report['checks']}['platform.storage'],'unknown')
            self.assertIsNone(report['storage']['mmc_error_counts'])
            self.assertNotIn('PRIVATE-CANARY',json.dumps(report))
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
            stats=root/'sys/kernel/debug/mmc0/err_stats'; stats.parent.mkdir(parents=True)
            stats.write_text(MMC_ZERO.replace('Command Timeout Occurred:\t 0','Command Timeout Occurred:\t 2'))
            before={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file()}
            def runner(args,**kwargs): return subprocess.CompletedProcess(args,1,'','not available')
            report=diagnostics.collect(root,'minimal',runner)
            after={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in root.rglob('*') if p.is_file()}
            self.assertEqual(before,after)
            self.assertNotIn('sentinel',json.dumps(report))
            self.assertEqual(report['storage']['mmc_error_counts']['command_timeout'],2)
            self.assertEqual({item['id']:item['status'] for item in report['checks']}['platform.storage'],'warning')

if __name__=='__main__': unittest.main()
