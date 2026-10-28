"""Synthetic local control/transaction tests; no Pi hardware qualification."""
import io
import json
import os
from pathlib import Path
import socket
import signal
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from calibration_test_support import prepare_root, bundle, write_json
from calibration_import_test import frames

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import nuvo_calibration_broker as broker
import nuvo_calibration_control as control
from nuvo_calibration_store import exclusive_lock


def archive(source, entries=None):
    result = io.BytesIO()
    with tarfile.open(fileobj=result, mode='w', format=tarfile.USTAR_FORMAT) as output:
        if entries is None:
            entries = [(name, (source / name).read_bytes()) for name in broker.FILES]
        for name, data in entries:
            member = tarfile.TarInfo(name)
            if data is None:
                member.type = tarfile.SYMTYPE
                member.linkname = '/etc/passwd'
                output.addfile(member)
            else:
                member.size = len(data)
                output.addfile(member, io.BytesIO(data))
    return result.getvalue()


@unittest.skipUnless(os.name == 'posix', 'native control uses Linux/POSIX')
class BrokerTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.cfg = prepare_root(self.root)
        self.source, self.manifest = bundle(self.root)
        (self.source.parent / 'active').unlink()
        self.restarts = 0
        self.fail_restart = False
        self.wrong_frame = False

    def native(self, args, **kwargs):
        if '--dump-effective-config' in args:
            return subprocess.CompletedProcess(args, 0, json.dumps({'configuration':self.cfg}), '')
        if '--calibration-check' in args:
            return subprocess.CompletedProcess(args, 0, '{"valid":true}', '')
        if args[0] == 'systemctl':
            if args[1] == 'restart':
                self.restarts += 1
                if self.fail_restart:
                    self.fail_restart = False
                    return subprocess.CompletedProcess(args, 1, '', 'injected restart failure')
                selected = control.selected_calibration(self.root, self.native)
                write_json(self.root/'run/nuvo-depth/status.json', {
                    'status':'running', 'run_uuid':f'{self.restarts:032x}',
                    'calibration_sha256':selected['bundle_id'] if selected else 'e'*64,
                    'calibration_authority':1 if selected else 0})
            return subprocess.CompletedProcess(args, 0, '', '')
        if str(args[0]).endswith('nuvo-depth-read'):
            health = control.daemon_health(self.root)
            value = 'f'*64 if self.wrong_frame else health['calibration_sha256']
            self.wrong_frame = False
            return subprocess.CompletedProcess(args, 0,
                frames(value, health['run_uuid'], health['calibration_authority']), b'')
        raise AssertionError(args)

    def request(self, data, header=None):
        client, server = socket.socketpair()
        errors = []
        def serve():
            try:
                with server:
                    broker.handle(server, self.root, self.native)
            except BaseException as error:
                errors.append(error)
        worker = threading.Thread(target=serve)
        worker.start()
        with client:
            client.settimeout(5)
            client.sendall(header if header is not None else b'NVC1'+struct.pack('>Q', len(data))+data)
            client.shutdown(socket.SHUT_WR)
            response = bytearray()
            while len(response) < 8:
                block = client.recv(8-len(response))
                if not block:
                    raise RuntimeError('broker closed without reply: '+repr(errors))
                response.extend(block)
            self.assertEqual(response[:4], b'NCA1')
            size = struct.unpack_from('>I', response, 4)[0]
            payload = bytearray()
            while len(payload) < size:
                block = client.recv(size-len(payload))
                if not block:
                    raise RuntimeError('broker truncated reply: '+repr(errors))
                payload.extend(block)
        worker.join(5)
        self.assertFalse(worker.is_alive())
        if errors:
            raise errors[0]
        return json.loads(payload)

    def test_import_then_same_bundle_verifies_without_restart_or_new_transaction(self):
        data = archive(self.source)
        first = self.request(data)
        self.assertTrue(first['ok'])
        self.assertEqual(first['calibration_sha256'], self.manifest['bundle_id'])
        self.assertEqual(first['calibration_authority'], 1)
        journals = list((self.root/control.STATE/'transactions').glob('*.json'))
        second = self.request(data)
        self.assertTrue(second['unchanged'])
        self.assertEqual(first['run_uuid'], second['run_uuid'])
        self.assertEqual(self.restarts, 1)
        self.assertEqual(journals, list((self.root/control.STATE/'transactions').glob('*.json')))
        self.wrong_frame = True
        rejected = self.request(data)
        self.assertEqual(rejected['code'], 'activation_failed')
        self.assertEqual(self.restarts, 1)
        self.assertEqual(os.readlink(self.source.parent/'active'), self.manifest['bundle_id'])

    def test_same_selected_bundle_with_different_loaded_native_hash_restarts(self):
        data = archive(self.source)
        self.request(data)
        health = control.daemon_health(self.root)
        health['calibration_sha256'] = 'e'*64
        write_json(self.root/'run/nuvo-depth/status.json', health)
        result = self.request(data)
        self.assertTrue(result['ok'])
        self.assertFalse(result['unchanged'])
        self.assertEqual(self.restarts, 2)

    def test_bounds_protocol_and_truncated_upload_fail_before_mutation(self):
        for header in [b'BAD!'+struct.pack('>Q', 1024),
                       b'NVC1'+struct.pack('>Q', broker.MAX_UPLOAD_BYTES+1),
                       b'NVC1'+struct.pack('>Q', 1024)+b'partial', b'NVC']:
            with self.subTest(header=header[:12]):
                self.assertEqual(self.request(b'', header)['code'], 'invalid_request')
        self.assertEqual(self.restarts, 0)
        self.assertFalse((self.root/control.MARKER).exists())

    def test_traversal_links_duplicates_extra_members_and_missing_assets_fail(self):
        originals = [(name, (self.source/name).read_bytes()) for name in broker.FILES]
        cases = [originals+[('../escape', b'bad')], originals+[('/absolute', b'bad')],
                 originals+[(broker.FILES[0], b'duplicate')], originals+[('extra', b'bad')],
                 originals[:-1], [(broker.FILES[0], None), *originals[1:]]]
        for entries in cases:
            with self.subTest(names=[name for name, _ in entries]):
                self.assertEqual(self.request(archive(self.source, entries))['code'], 'invalid_request')
        self.assertEqual(self.restarts, 0)
        self.assertFalse((self.root/'escape').exists())

    def test_extended_tar_headers_and_oversized_declared_files_fail_without_allocating(self):
        for kind in (tarfile.XHDTYPE, tarfile.GNUTYPE_LONGNAME, tarfile.GNUTYPE_SPARSE):
            member = tarfile.TarInfo('manifest.json')
            member.type = kind
            data = member.tobuf(format=tarfile.USTAR_FORMAT) + bytes(1024)
            self.assertEqual(self.request(data)['code'], 'invalid_request')
        member = tarfile.TarInfo('manifest.json')
        member.size = broker.MAX_FILE_BYTES+1
        data = member.tobuf(format=tarfile.USTAR_FORMAT) + bytes(1024)
        self.assertEqual(self.request(data)['code'], 'invalid_request')
        self.assertEqual(self.restarts, 0)

    def test_wrong_unit_and_corrupt_bundle_fail_existing_native_validation(self):
        source, _ = bundle(self.root, unit_id='another-unit')
        (source.parent/'active').unlink()
        result = self.request(archive(source))
        self.assertEqual(result['code'], 'invalid_bundle')
        self.assertIn('another device', result['message'])
        (self.source/'front_left.yaml').write_text('corrupt')
        self.assertEqual(self.request(archive(self.source))['code'], 'invalid_bundle')
        self.assertEqual(self.restarts, 0)

    def test_restart_failure_rolls_back_previous_native_selection_and_policy(self):
        self.request(archive(self.source))
        previous_policy = control.CapturePolicy(self.root).snapshot()['files']
        second, _ = bundle(self.root, provenance={'revision':2})
        (second.parent/'active').unlink()
        (second.parent/'active').symlink_to(self.manifest['bundle_id'])
        self.fail_restart = True
        result = self.request(archive(second))
        self.assertEqual(result['code'], 'activation_failed')
        self.assertIn('previous selection and service restored', result['message'])
        self.assertEqual(os.readlink(self.source.parent/'active'), self.manifest['bundle_id'])
        self.assertEqual(control.CapturePolicy(self.root).snapshot()['files'], previous_policy)
        self.assertEqual(control.daemon_health(self.root)['calibration_sha256'], self.manifest['bundle_id'])

    def test_active_transaction_returns_bounded_busy_error(self):
        with exclusive_lock(self.root/control.STATE/'.calibration.lock'):
            self.assertEqual(self.request(archive(self.source))['code'], 'busy')
        self.assertEqual(self.restarts, 0)

    def test_disconnect_after_full_upload_finishes_authorized_transaction(self):
        client, server = socket.socketpair()
        result = []
        def serve():
            with server:
                result.append(broker.handle(server, self.root, self.native))
        worker = threading.Thread(target=serve)
        worker.start()
        data = archive(self.source)
        with client:
            client.sendall(b'NVC1'+struct.pack('>Q', len(data))+data)
        worker.join(5)
        self.assertFalse(worker.is_alive())
        self.assertTrue(result[0]['ok'])
        self.assertEqual(control.daemon_health(self.root)['calibration_sha256'], self.manifest['bundle_id'])

    def test_service_stop_waits_for_active_operation(self):
        handlers = {}
        entered, release, finished = threading.Event(), threading.Event(), threading.Event()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            endpoint = self.root/'stop.sock'
            listener.bind(str(endpoint))
            listener.listen(1)
            def operation(_connection):
                entered.set()
                if not release.wait(3):
                    raise RuntimeError('test did not release operation')
                finished.set()
            with patch.object(broker, 'activated_listener', return_value=listener), \
                 patch.object(broker, 'handle', side_effect=operation), \
                 patch.object(broker.signal, 'signal', side_effect=lambda signum, handler: handlers.update({signum:handler})), \
                 patch.object(sys, 'argv', ['nuvo_calibration_broker.py']):
                worker = threading.Thread(target=broker.main)
                worker.start()
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.connect(str(endpoint))
                    self.assertTrue(entered.wait(2))
                    handlers[signal.SIGTERM](signal.SIGTERM, None)
                    self.assertTrue(worker.is_alive())
                    self.assertFalse(finished.is_set())
                    release.set()
                    worker.join(3)
                self.assertFalse(worker.is_alive())
                self.assertTrue(finished.is_set())

    def test_socket_permissions_and_service_installation_contract(self):
        unit = (ROOT/'deployment/systemd/nuvo-depth-calibration-control.socket').read_text()
        self.assertIn('SocketMode=0660', unit)
        self.assertIn('SocketUser=root', unit)
        self.assertIn('SocketGroup=nuvo-video', unit)
        self.assertIn('Backlog=4', unit)
        self.assertIn('After=systemd-tmpfiles-setup.service', unit)
        service = (ROOT/'deployment/systemd/nuvo-depth-calibration-control.service').read_text()
        self.assertIn('User=root', service)
        self.assertNotIn('RuntimeDirectory=', service)
        self.assertNotIn('ExecStopPost=', service)
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaises(RuntimeError):
                broker.activated_listener()

    def test_listener_refuses_world_writable_socket(self):
        import grp
        endpoint = self.root/'permission.sock'
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(str(endpoint))
            listener.listen(1)
            fake = SimpleNamespace(family=socket.AF_UNIX, type=socket.SOCK_STREAM,
                                   getsockname=lambda: str(endpoint))
            os.chmod(endpoint, 0o666)
            with patch.dict(os.environ, {'LISTEN_PID':str(os.getpid()), 'LISTEN_FDS':'1'}), \
                 patch.object(broker.socket, 'socket', return_value=fake), \
                 patch.object(broker.os, 'geteuid', return_value=0), \
                 patch.object(grp, 'getgrnam', return_value=SimpleNamespace(gr_gid=endpoint.stat().st_gid)):
                with self.assertRaisesRegex(RuntimeError, '0660'):
                    broker.activated_listener()


if __name__ == '__main__':
    unittest.main()
