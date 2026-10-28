"""Discover same-Pi container permissions without enabling capture or reading frames."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import nuvo_ros_environment as ros


class RosEnvironmentTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root/'etc/nuvo-depth').mkdir(parents=True)
        (self.root/'etc/group').write_text('nuvo-video:x:1234:\n')
        target = self.root/ros.CONTRACT
        target.parent.mkdir(parents=True)
        shutil.copyfile(ROOT/'deployment/ros-local.json', target)
        unit = self.root/'usr/lib/systemd/system/nuvo-depthd-native.service'
        unit.parent.mkdir(parents=True)
        unit.write_text('[Unit]\nConditionPathExists=/etc/nuvo-depth/capture-enabled\n'
                        '[Service]\nExecStart=/usr/bin/depthd --run /etc/nuvo-depth/daemon.yaml\n')

    def test_disabled_capture_and_existing_identity_are_preserved_by_discovery(self):
        config = self.root/'etc/nuvo-depth/stereo.json'; config.write_text('{"fixture":"preserved"}\n')
        identity = self.root/'var/lib/nuvo-depth/identity/device.uuid'
        identity.parent.mkdir(parents=True); identity.write_text('existing-unit\n')
        before = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        value = ros.environment(self.root)
        self.assertEqual(value['access_gid'], 1234)
        self.assertTrue(value['matching_grid_geometry'])
        self.assertFalse(value['capture_enabled'])
        self.assertFalse(value['data_ready'])
        self.assertIn('capture_disabled', value['reasons'])
        self.assertEqual({p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}, before)

    def test_missing_or_malformed_contract_never_claims_new_package_capabilities(self):
        for data in (None, '[]', '{', json.dumps({'schema_version': 2})):
            with self.subTest(data=data):
                path = self.root/ros.CONTRACT
                if data is None: path.unlink(missing_ok=True)
                else: path.write_text(data)
                value = ros.environment(self.root)
                self.assertFalse(value['matching_grid_geometry'])
                self.assertIsNone(value['calibration_control'])
                self.assertIn('native_package_upgrade_required', value['reasons'])

    @unittest.skipUnless(os.name == 'posix' and hasattr(socket, 'AF_UNIX'), 'Unix socket permissions required')
    def test_numeric_gid_socket_type_and_group_permissions_are_checked(self):
        gid = os.getgid()
        (self.root/'etc/group').write_text(f'nuvo-video:x:{gid}:\n')
        runtime = self.root/ros.RUNTIME; runtime.mkdir(parents=True); runtime.chmod(0o750)
        servers = []
        for name in ('data.sock', 'calibration.sock'):
            server = socket.socket(socket.AF_UNIX); servers.append(server); self.addCleanup(server.close)
            server.bind(str(runtime/name)); (runtime/name).chmod(0o660)
        (self.root/'etc/nuvo-depth/capture-enabled').write_text('existing-policy\n')
        status = runtime/'status.json'; status.write_text('{"status":"running"}')
        value = ros.environment(self.root)
        self.assertTrue(value['data_ready']); self.assertTrue(value['control_ready'])
        self.assertEqual(value['reasons'], [])
        before = time.time() - 30; os.utime(status, (before, before))
        self.assertFalse(ros.environment(self.root)['data_ready'])
        self.assertIn('daemon_not_running', ros.environment(self.root)['reasons'])
        status.write_text('{"status":"running"}')
        (runtime/'data.sock').chmod(0o600)
        self.assertFalse(ros.environment(self.root)['data_ready'])
        (runtime/'data.sock').unlink(); (runtime/'data.sock').write_text('not a socket')
        self.assertFalse(ros.environment(self.root)['checks']['data_socket']['present'])
        (self.root/'etc/group').write_text(f'nuvo-video:x:{gid+1}:\n')
        self.assertFalse(ros.environment(self.root)['control_ready'])

    def test_legacy_unguarded_full_service_does_not_require_minimal_marker(self):
        unit = self.root/'usr/lib/systemd/system/nuvo-depthd-native.service'
        unit.write_text('[Service]\nExecStart=/usr/bin/depthd --run /etc/nuvo-depth/daemon.yaml\n')
        self.assertTrue(ros.environment(self.root)['capture_enabled'])
        self.assertNotIn('capture_disabled', ros.environment(self.root)['reasons'])

    def test_both_terminal_clis_expose_machine_readable_discovery(self):
        for name in ('nuvo_minimal_cli.py', 'nuvoctl.py'):
            with self.subTest(cli=name):
                output = subprocess.check_output([sys.executable, str(ROOT/'tools'/name), '--root',
                                                  str(self.root), 'ros', 'environment', '--json'], text=True)
                self.assertEqual(json.loads(output)['access_gid'], 1234)

    @unittest.skipUnless(os.name == 'posix' and os.geteuid() == 0 and
                         shutil.which('systemd-sysusers') and shutil.which('systemd-tmpfiles'),
                         'systemd directory provisioning requires a Linux root fixture')
    def test_package_tmpfiles_prepare_runtime_access_and_writable_policy_directory(self):
        for profile in ('nuvo-depth-minimal', 'nuvo-depth'):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); (root/'etc').mkdir()
                (root/'etc/group').write_text('root:x:0:\nnuvo-video:x:1234:\n')
                (root/'etc/passwd').write_text('root:x:0:0:root:/root:/bin/sh\n')
                for program, suffix in (('systemd-sysusers', 'sysusers'), ('systemd-tmpfiles', 'tmpfiles')):
                    command = [program, '--root', str(root)]
                    if suffix == 'tmpfiles': command.append('--create')
                    command.append(str(ROOT/'deployment/systemd'/f'{profile}.{suffix}.conf'))
                    subprocess.run(command, check=True, capture_output=True, text=True)
                runtime = (root/ros.RUNTIME).stat()
                self.assertEqual(runtime.st_gid, 1234)
                self.assertEqual(runtime.st_mode & 0o777, 0o750)
                policy = (root/'etc/systemd/system/nuvo-depthd-native.service.d').stat()
                self.assertEqual((policy.st_uid, policy.st_gid, policy.st_mode & 0o777), (0, 0, 0o755))

    @unittest.skipUnless(shutil.which('systemd-analyze'), 'systemd unit verifier required')
    def test_calibration_socket_and_service_pass_systemd_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            units = []
            for suffix in ('socket', 'service'):
                name = f'nuvo-depth-calibration-control.{suffix}'
                target = Path(directory)/name
                target.write_bytes((ROOT/'deployment/systemd'/name).read_bytes())
                units.append(str(target))
            subprocess.run(['systemd-analyze', 'verify', *units], check=True, capture_output=True, text=True)


if __name__ == '__main__': unittest.main()
