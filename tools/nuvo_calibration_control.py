"""Shared calibration readiness and device control, without image-solving imports."""
from __future__ import annotations

import base64
import copy
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import shutil
import struct
import tempfile
import time

from nuvo_calibration_store import CalibrationStore, REQUIRED_ASSETS, atomic_bytes, atomic_json, exclusive_lock, fsync_directory, read_transaction, validate_bundle

STATE = Path('var/lib/nuvo-depth')
CONFIG = Path('etc/nuvo-depth/daemon.yaml')
MARKER = Path('etc/nuvo-depth/capture-enabled')
SERVICE = 'nuvo-depthd-native.service'
POLICY_OVERRIDE = Path('etc/systemd/system') / (SERVICE + '.d/90-calibration-policy.conf')


def read_json(path, limit=65536):
    if not path.is_file() or path.stat().st_size > limit:
        raise ValueError(f'Missing or oversized status/configuration: {path}')
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError(f'Expected a JSON object: {path}')
    return value


def daemon_health(root, run=subprocess.run):
    path = root / 'run/nuvo-depth/status.json'
    try:
        health = read_json(path)
        if not isinstance(health.get('status'), str):
            raise ValueError('Daemon status needs a status string')
        authority = health.get('calibration_authority', 0)
        if type(authority) is not int or authority not in (0, 1, 2):
            raise ValueError('Daemon calibration authority must be 0, 1 or 2')
        for key in ('run_uuid', 'device_uuid', 'calibration_sha256'):
            if key in health and not isinstance(health[key], str):
                raise ValueError('Daemon calibration/identity metadata is malformed')
        if 'capture_sequence' in health and (type(health['capture_sequence']) is not int or health['capture_sequence'] < 0):
            raise ValueError('Daemon capture sequence is malformed')
        if time.time() - path.stat().st_mtime > 15:
            return {'status': 'unavailable', 'error': 'Daemon status is stale', 'error_code': 'daemon_status_stale'}
        # A just-stopped service can leave a recent status file behind. That
        # file must not establish current loaded/fresh-frame verification.
        if health['status'] == 'running' and (root/'run/systemd/system').is_dir():
            try:
                service = run(['systemctl','is-active',SERVICE],capture_output=True,text=True,timeout=5,check=False)
            except (OSError,subprocess.SubprocessError):
                return {'status':'unavailable','error':'Capture service activity could not be determined',
                        'error_code':'daemon_service_unavailable'}
            if service.returncode or service.stdout.strip() != 'active':
                value=service.stdout.strip()
                return {'status':'unavailable','error':'Capture service is not active; request fresh output after starting capture',
                        'error_code':'daemon_service_not_running',
                        'service_state':value if value in ('inactive','failed','activating','deactivating') else 'unknown'}
        return health
    except (OSError, ValueError) as error:
        return {'status': 'unavailable', 'error': str(error),
                'error_code': 'daemon_status_missing' if not path.exists() else 'daemon_status_malformed'}


def service_configuration(root, run=subprocess.run):
    directories = ['usr/lib/systemd/system', 'lib/systemd/system', 'etc/systemd/system']
    units = [root / directory / SERVICE for directory in directories]
    units = [path for path in units if path.is_file()]
    if not units:
        raise ValueError('The capture service is not installed')
    base = units[-1].read_text()
    dropins = {}
    for directory in directories:
        for path in (root / directory / (SERVICE + '.d')).glob('*.conf'):
            dropins[path.name] = path
    texts = [base] + [dropins[name].read_text() for name in sorted(dropins)]
    command = None
    guarded = False
    for text in texts:
        for line in text.splitlines():
            if line.startswith('ExecStart='):
                command = shlex.split(line.split('=', 1)[1]) or None
            if line.startswith('ConditionPathExists=') and 'capture-enabled' in line:
                guarded = True
    if root == Path('/') and (root / 'run/systemd/system').is_dir():
        result = run(['systemctl', 'show', SERVICE, '--property=ExecStart', '--value'],
                     capture_output=True, text=True, timeout=10, check=False)
        match = re.search(r'argv\[\]=(.*?)\s*;', result.stdout)
        if result.returncode or not match:
            raise ValueError('Cannot determine the effective capture service command')
        command = shlex.split(match.group(1))
    if not command or Path(command[0]).name != 'depthd' or '--run' not in command:
        raise ValueError('Capture uses an unsupported service command; inspect its override')
    return {'command': command, 'preview_enabled': '--allow-provisional-depth' in command,
            'capture_policy_enabled': not guarded or (root / MARKER).is_file()}


def configuration(root, run=subprocess.run):
    binary = root / 'usr/bin/depthd'
    command = str(binary) if binary.is_file() else 'depthd'
    result = run([command, '--dump-effective-config', str(root / CONFIG)],
                 capture_output=True, text=True, timeout=10, check=False)
    if result.returncode:
        raise ValueError(result.stderr.strip() or 'Cannot validate the capture configuration')
    return json.loads(result.stdout)['configuration']


def validate_compatibility(root, bundle, namespace, run=subprocess.run, cfg=None):
    manifest = validate_bundle(bundle, require_factory_signature=namespace == 'factory',
                               trusted_keys=root / 'etc/nuvo-depth/factory-trust.json')
    if (namespace == 'factory') != (manifest['authority'] == 'factory_validated'):
        raise ValueError('Calibration namespace and authority disagree')
    unit = (root / STATE / 'identity/device.uuid').read_text().strip()
    if not unit or manifest['unit_id'] != unit:
        raise ValueError('Calibration belongs to another device')
    cfg = cfg if cfg is not None else configuration(root, run)
    if cfg['product']['platform'] != 'raspberry-pi-5' or cfg['rig']['sensor'] != 'imx296':
        raise ValueError('Calibration compatibility requires a supported hardware configuration')
    if manifest['preset_id'] != cfg['product']['profile_id']:
        raise ValueError('Calibration capture profile does not match the configured profile')
    contract = read_json(root / 'usr/share/nuvo-depth/calibration/profile.json')
    for key in ['left_camera_id', 'right_camera_id']:
        if manifest[key] != contract[key]:
            raise ValueError(f'Calibration {key} does not match the supported camera mapping')
    if cfg['capture']['left_camera_index'] != 0 or cfg['capture']['right_camera_index'] != 1:
        raise ValueError('Configured camera mapping is outside the supported calibration contract')
    profile = manifest.get('profile')
    if profile:
        if (profile['image_width'], profile['image_height']) != (cfg['capture']['width'], cfg['capture']['height']):
            raise ValueError('Calibration dimensions do not match the capture geometry')
        if not any(abs(rate - cfg['capture']['target_hz']) < 1e-6 for rate in profile['frame_rates_hz']):
            raise ValueError('Calibration does not list the configured capture cadence')
    binary = root / 'usr/bin/depthd'
    result = run([str(binary) if binary.is_file() else 'depthd', '--calibration-check',
                  str(root / CONFIG), str(bundle), str(root / STATE / 'identity/device.uuid'), namespace],
                 capture_output=True, text=True, timeout=15, check=False)
    if result.returncode:
        raise ValueError(result.stderr.strip() or 'Native calibration geometry validation failed')
    return manifest


def selected_calibration(root, run=subprocess.run, cfg=None):
    for namespace in ['factory', 'user']:
        active = root / STATE / 'calibrations' / namespace / 'active'
        if active.exists() or active.is_symlink():
            return validate_compatibility(root, active, namespace, run, cfg)
    return None


def readiness(root=Path('/'), run=subprocess.run):
    root = Path(root)
    health = daemon_health(root,run)
    result = {'schema_version': 1, 'state': 'unavailable', 'label': 'Status unavailable/error',
              'message': 'Calibration readiness could not be determined.',
              'next_action': 'Inspect the specific problem and repair the configuration or service.',
              'accepted': False, 'preview_enabled': False, 'capture_enabled': False,
              'bundle_id': None, 'loaded': None,
              'authority': None, 'source': None, 'calibration_sha256': None,
              'bindings': None, 'reason': 'readiness_unavailable',
              'runtime_verification': {'state': 'unverified', 'run_uuid': health.get('run_uuid'),
                                       'fresh_frames_verified': False},
              'camera_service': {key: health.get(key) for key in
                                 ['status', 'run_uuid', 'capture_sequence', 'error', 'error_code', 'service_state']}}
    try:
        if health.get('error_code') == 'daemon_status_malformed':
            result['reason'] = 'daemon_status_malformed'
            raise ValueError('Daemon status is malformed; inspect the capture service and its status file. ' + health['error'])
        for journal in (root/STATE/'transactions').glob('calibration-*.json'):
            if read_transaction(journal).get('state') in {'staging','awaiting_restart','restoring'}:
                result['reason'] = 'activation_pending'
                raise ValueError('Calibration activation or recovery is pending. Wait for the active command; if it was interrupted, retry import to restore the previous selection, then retry the requested change.')
        policy = service_configuration(root, run)
        result.update(preview_enabled=policy['preview_enabled'],
                      capture_enabled=policy['capture_policy_enabled'])
        cfg = configuration(root, run)
        manifest = selected_calibration(root, run, cfg)
        if manifest:
            result['bundle_id'] = manifest['bundle_id']
            result.update(authority=manifest['authority'], source='factory_bundle' if manifest['authority'] == 'factory_validated' else 'user_bundle',
                          calibration_sha256=manifest['bundle_id'],
                          bindings={key: manifest.get(key) for key in
                                    ('unit_id', 'left_camera_id', 'right_camera_id', 'preset_id', 'profile')})
            if health.get('status') == 'running':
                result['loaded'] = health.get('calibration_sha256') == manifest['bundle_id']
                expected_authority = 2 if manifest['authority'] == 'factory_validated' else 1
                if not result['loaded'] or health.get('calibration_authority') != expected_authority:
                    result['reason'] = 'loaded_calibration_mismatch'
                    raise ValueError('The selected accepted bundle is not the calibration loaded by the daemon')
                result['runtime_verification']['state'] = 'daemon_loaded'
                receipt_path = root/STATE/'calibration-verification.json'
                if receipt_path.exists() and result['capture_enabled']:
                    receipt = read_json(receipt_path)
                    sequences = receipt.get('capture_sequences')
                    if (receipt.get('schema_version') != 1 or not isinstance(sequences,list) or len(sequences)!=2
                            or any(type(value) is not int or value<0 for value in sequences) or sequences[0]>=sequences[1]):
                        raise ValueError('Calibration verification receipt is malformed; verify fresh output again')
                    if (receipt.get('bundle_id') == manifest['bundle_id'] and receipt.get('run_uuid') == health.get('run_uuid')
                            and receipt.get('authority') == expected_authority):
                        result['runtime_verification'].update(state='fresh_frames_verified', fresh_frames_verified=True,
                                                              verified_at_unix_ns=receipt.get('verified_at_unix_ns'),
                                                              capture_sequences=sequences)
            result.update(state='calibration_accepted', label='Calibration accepted', accepted=True,
                          reason='matching_bundle_validated',
                          message='A matching per-unit calibration passed its declared validation. This is not independent metric-accuracy or production qualification.',
                          next_action='Inspect calibration details and verify fresh output; check camera/service health separately.')
            if not result['capture_enabled']:
                result['message'] += ' Capture is disabled by policy.'
                result['next_action'] = 'Import this compatible bundle to enable capture and verify fresh output; camera/service health is separate.'
        elif health.get('status') == 'running' and health.get('calibration_authority', 0) > 0:
            raise ValueError('Existing validated calibration files are preserved, but a matching per-unit bundle cannot be verified. Import the original compatible bundle; a software upgrade alone does not require recalibration.')
        elif policy['preview_enabled'] and policy['capture_policy_enabled']:
            result.update(state='preview_enabled', label='Preview enabled',
                          authority='provisional_recalibration_required', source='provisional_seed',
                          reason='preview_policy_enabled', calibration_sha256=health.get('calibration_sha256'),
                          message='Provisional output is enabled; distance accuracy remains unverified. Camera/service health is separate.',
                          next_action='Continue engineering evaluation or import an accepted calibration bundle.')
        elif policy['capture_policy_enabled']:
            raise ValueError('Capture policy is enabled without a verifiable active bundle or explicit preview. Existing calibration files are preserved; inspect the service override and import the original compatible bundle. A software upgrade alone does not require recalibration.')
        else:
            result.update(state='calibration_needed', label='Calibration needed', capture_enabled=False,
                          reason='calibration_or_preview_required',
                          message='Capture is waiting for calibration or an explicit provisional-preview selection.',
                          next_action='Import a compatible accepted bundle, or deliberately enable provisional preview.')
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        result['problem'] = str(error)
        result['message'] = str(error)
    return result


def status(root=Path('/'), run=subprocess.run):
    root = Path(root)
    return {**daemon_health(root,run), 'calibration_readiness': readiness(root, run)}


class CapturePolicy:
    """Only these two managed files may be restored from a transaction journal."""
    def __init__(self, root):
        self.root = Path(root)
        self.applied = None

    def snapshot(self):
        files = {}
        for relative in [MARKER, POLICY_OVERRIDE]:
            path = self.root / relative
            if path.is_symlink() or (path.exists() and (not path.is_file() or path.stat().st_size > 65536)):
                raise ValueError(f'Unsupported capture policy file: {path}')
            files[str(relative)] = base64.b64encode(path.read_bytes()).decode() if path.exists() else None
        health = daemon_health(self.root)
        return {'files': files, 'runtime': {key: health.get(key) for key in ['calibration_sha256', 'calibration_authority']}}

    def accepted(self, snapshot):
        target = copy.deepcopy(snapshot)
        target['files'][str(MARKER)] = base64.b64encode(b'accepted-calibration\n').decode()
        return target

    def apply(self, snapshot):
        if set(snapshot['files']) != {str(MARKER), str(POLICY_OVERRIDE)}:
            raise ValueError('Unsafe capture-policy journal')
        for relative, encoded in snapshot['files'].items():
            path = self.root / relative
            if encoded is None:
                path.unlink(missing_ok=True)
            else:
                data = base64.b64decode(encoded, validate=True)
                if len(data) > 65536:
                    raise ValueError('Oversized capture-policy journal')
                path.parent.mkdir(parents=True, exist_ok=True)
                atomic_bytes(path, data)
                os.chmod(path, 0o644 if relative == str(POLICY_OVERRIDE) else 0o600)
            if path.parent.exists():
                fsync_directory(path.parent)
        self.applied = snapshot


def fresh_frame(root, expected, authority, run_uuid, run=subprocess.run):
    """Exercise demand-driven depth, then bind fresh NUV1 metadata to the loaded bundle."""
    reader = root / 'usr/lib/nuvo-depth/nuvo-depth-read'
    result = run([str(reader), '--socket', str(root / 'run/nuvo-depth/data.sock'),
                  '--frames', '2', '--hz', '2', '--timeout-seconds', '30'],
                 capture_output=True, timeout=40, check=False)
    if result.returncode:
        raise RuntimeError('Fresh depth verification failed: ' + result.stderr.decode(errors='replace').strip())
    data = result.stdout
    sequences = []
    while data:
        if len(data) < 32:
            raise RuntimeError('Truncated depth record')
        metadata_size = struct.unpack_from('>I', data, 12)[0]
        payload_size = struct.unpack_from('>Q', data, 16)[0]
        size = 32 + metadata_size + payload_size
        if not 32 < size <= 64*1024*1024 or len(data) < size:
            raise RuntimeError('Invalid depth record bounds')
        frame, data = data[:size], data[size:]
        magic, major, minor, kind, flags, metadata_size, payload_size, sequence = struct.unpack_from('>4sHHHHIQQ', frame)
        if ((magic, major, minor, kind, flags) != (b'NUV1',1,0,5,0) or not 0 < metadata_size <= 65536
                or len(frame) != 32+metadata_size+payload_size):
            raise RuntimeError('Invalid depth frame header')
        metadata = json.loads(frame[32:32+metadata_size])
        if (metadata.get('calibration_sha256') != expected or metadata.get('calibration_authority') != authority
                or metadata.get('run_uuid', '').replace('-', '') != run_uuid.replace('-', '')
                or metadata.get('capture_sequence') != sequence or metadata.get('depth_unit') != 'metres'
                or {item.get('kind') for item in metadata.get('products', [])} != {'depth','validity'}):
            raise RuntimeError('Fresh depth metadata does not match the activated calibration and daemon run')
        sequences.append(sequence)
    if len(sequences) != 2 or sequences[0] >= sequences[1]:
        raise RuntimeError('Depth frames did not advance')
    return sequences


def restart_and_verify(root, policy, expected_bundle, run=subprocess.run):
    def systemctl(*args, timeout=70):
        result = run(['systemctl', *args], capture_output=True, text=True, timeout=timeout, check=False)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or 'Capture service command failed: ' + ' '.join(args))
    old_run = daemon_health(root,run).get('run_uuid')
    systemctl('daemon-reload')
    configuration_state = service_configuration(root, run)
    if not configuration_state['capture_policy_enabled']:
        systemctl('stop', SERVICE)
        return
    systemctl('restart', SERVICE)
    systemctl('is-active', '--quiet', SERVICE, timeout=5)
    health = daemon_health(root,run)
    if health.get('status') != 'running' or not health.get('run_uuid') or health['run_uuid'] == old_run:
        raise RuntimeError('Capture did not publish a new running daemon status')
    expected = expected_bundle
    authority = None
    if expected:
        manifest = selected_calibration(root, run)
        if not manifest or manifest['bundle_id'] != expected:
            raise RuntimeError('Selected calibration changed during activation')
        authority = 2 if manifest['authority'] == 'factory_validated' else 1
    else:
        previous_runtime = (policy.applied or {}).get('runtime', {})
        expected = previous_runtime.get('calibration_sha256')
        authority = previous_runtime.get('calibration_authority')
        # A previous disabled installation is stopped above. Existing preview
        # or legacy geometry must be restored byte-for-byte when it was running.
        if expected is None and configuration_state['preview_enabled']:
            expected, authority = health.get('calibration_sha256'), 0
    if not expected or health.get('calibration_sha256') != expected or health.get('calibration_authority') != authority:
        raise RuntimeError('Capture loaded a different calibration than the requested selection')
    sequences = fresh_frame(root, expected, authority, health['run_uuid'], run)
    atomic_json(root/STATE/'calibration-verification.json',
                {'schema_version':1,'bundle_id':expected,'authority':authority,'run_uuid':health['run_uuid'],
                 'capture_sequences':sequences,'verified_at_unix_ns':time.time_ns()})
    return sequences


def import_bundle(root, source, dry_run=False, run=subprocess.run, *,
                  idempotent=False, lock_timeout=None):
    root, source = Path(root), Path(source)
    if source.is_symlink() or not source.is_dir():
        raise ValueError('Select an extracted calibration bundle directory, not a link or archive')
    # Private copies prevent an unprivileged caller changing assets after the
    # validation that authorizes privileged activation. Names and sizes are bounded.
    with tempfile.TemporaryDirectory(prefix='nuvo-calibration-import-') as temporary:
        staging = Path(temporary) / 'input'
        staging.mkdir()
        for name in ['manifest.json', *REQUIRED_ASSETS]:
            path = source / name
            if path.is_symlink() or not path.is_file() or path.stat().st_size > 4*1024*1024:
                raise ValueError(f'Missing, linked or oversized calibration file: {name}')
            with path.open('rb') as input_file:
                data = input_file.read(4*1024*1024+1)
            if len(data) > 4*1024*1024:
                raise ValueError(f'Oversized calibration file: {name}')
            (staging / name).write_bytes(data)
        manifest = validate_bundle(staging, trusted_keys=root/'etc/nuvo-depth/factory-trust.json')
        namespace = 'factory' if manifest['authority'] == 'factory_validated' else 'user'
        canonical = staging.with_name(manifest['bundle_id'])
        staging.rename(canonical)
        validate_compatibility(root, canonical, namespace, run)
        factory = root/STATE/'calibrations/factory/active'
        if namespace == 'user' and (factory.exists() or factory.is_symlink()):
            raise ValueError('Factory calibration has runtime precedence; user import cannot replace it')
        active = root/STATE/'calibrations'/namespace/'active'
        change = {'operation':'calibration_import','namespace':namespace,'previous':os.readlink(active) if active.is_symlink() else None,
                  'bundle_id':manifest['bundle_id'],'authority':manifest['authority'],'unit_id':manifest['unit_id'],
                  'capture':'Enable accepted calibration; verify a new daemon run and two advancing depth/validity frames.',
                  'accuracy':'Declared bundle validation is not independent metric-accuracy or production qualification.', 'dry_run':dry_run}
        if dry_run:
            return change
        print(json.dumps({'planned_change':change}, sort_keys=True), file=sys.stderr, flush=True)
        policy = CapturePolicy(root)
        store = CalibrationStore(root/STATE, root/'etc/nuvo-depth/factory-trust.json', policy)
        sequences = []
        verified_runtime = {}
        unchanged = False
        def restart(expected):
            sequences[:] = restart_and_verify(root, policy, expected, run) or []
            verified_runtime.update(daemon_health(root, run))
        def verify_existing(expected):
            nonlocal unchanged
            current = selected_calibration(root, run)
            health = daemon_health(root, run)
            authority = 2 if manifest['authority'] == 'factory_validated' else 1
            if (not current or current['bundle_id'] != expected or
                    not service_configuration(root, run)['capture_policy_enabled'] or
                    health.get('status') != 'running' or not health.get('run_uuid') or
                    health.get('calibration_sha256') != expected or
                    health.get('calibration_authority') != authority):
                return False
            sequences[:] = fresh_frame(root, expected, authority, health['run_uuid'], run)
            atomic_json(root/STATE/'calibration-verification.json',
                        {'schema_version':1,'bundle_id':expected,'authority':authority,
                         'run_uuid':health['run_uuid'],'capture_sequences':sequences,
                         'verified_at_unix_ns':time.time_ns()})
            unchanged = True
            verified_runtime.update(health)
            return True
        store.activate(canonical, namespace, restart=restart,
                       verify_existing=verify_existing if idempotent else None,
                       lock_timeout=lock_timeout)
        return {**change,'activated':True,'unchanged':unchanged,
                'verified_runtime':verified_runtime,
                'verified_capture_sequences':sequences,'readiness':readiness(root,run)}


def rollback(root, namespace=None, run=subprocess.run):
    root = Path(root)
    if namespace is None:
        namespace = 'factory' if (root/STATE/'calibrations/factory/active').is_symlink() else 'user'
    policy = CapturePolicy(root)
    store = CalibrationStore(root/STATE, root/'etc/nuvo-depth/factory-trust.json', policy)
    selected = store.rollback(namespace, restart=lambda expected: restart_and_verify(root,policy,expected,run))
    return {'rolled_back':True,'bundle_id':selected,'readiness':readiness(root,run)}


def preview_policy(root, enable, before, run=subprocess.run):
    target = copy.deepcopy(before)
    command = [arg for arg in service_configuration(root,run)['command'] if arg != '--allow-provisional-depth']
    if enable:
        command.append('--allow-provisional-depth')
    content = '[Service]\nExecStart=\nExecStart=' + shlex.join(command) + '\n'
    target['files'][str(POLICY_OVERRIDE)] = base64.b64encode(content.encode()).decode()
    target['files'][str(MARKER)] = base64.b64encode(b'explicit-preview\n').decode() if enable else None
    return target


def preview(root, enable, run=subprocess.run):
    root = Path(root)
    with exclusive_lock(root/STATE/'.calibration.lock'):
        policy = CapturePolicy(root)
        store = CalibrationStore(root/STATE,root/'etc/nuvo-depth/factory-trust.json',policy)
        if store._recover_pending(lambda expected: restart_and_verify(root,policy,expected,run)):
            raise RuntimeError('Interrupted activation restored; retry the preview selection')
        before = policy.snapshot()
        target = preview_policy(root, enable, before, run)
        namespace = 'factory' if (root/STATE/'calibrations/factory/active').is_symlink() else 'user'
        active = root/STATE/'calibrations'/namespace/'active'
        current = os.readlink(active) if active.is_symlink() else None
        transaction = {'schema_version':1,'operation':'calibration_preview','namespace':namespace,
                       'bundle_id':current,'previous':current,'policy_before':before,'policy_after':target,
                       'state':'staging','started_unix_ns':time.time_ns()}
        from nuvo_calibration_store import atomic_json
        journal = store.transactions / f'calibration-{time.time_ns()}.json'
        atomic_json(journal,transaction)
        store._complete_transition(journal,transaction,lambda expected:restart_and_verify(root,policy,expected,run))
    return readiness(root,run)


def recover(root=Path('/'), startup=False):
    root = Path(root)
    recovered = CalibrationStore(root/STATE,root/'etc/nuvo-depth/factory-trust.json',CapturePolicy(root)).recover_before_start()
    if startup and not (root/MARKER).is_file():
        raise RuntimeError('Capture policy is disabled after interrupted-activation recovery')
    return {'recovered':recovered}


def add_commands(commands):
    calibration = commands.add_parser('calibration', help='Calibration readiness, verified bundle import and rollback').add_subparsers(dest='operation',required=True)
    calibration.add_parser('status')
    load = calibration.add_parser('import')
    load.add_argument('bundle',type=Path)
    load.add_argument('--dry-run',action='store_true')
    back = calibration.add_parser('rollback')
    back.add_argument('--namespace',choices=['user','factory'])
    choice = calibration.add_parser('preview',help='Explicitly enable provisional preview or disable capture')
    flags = choice.add_mutually_exclusive_group(required=True)
    flags.add_argument('--enable',action='store_true');flags.add_argument('--disable',action='store_true')


def dispatch(args, root, run=subprocess.run):
    if args.operation == 'status':
        return readiness(root,run)
    if os.geteuid() != 0 and not (args.operation == 'import' and args.dry_run):
        raise ValueError('Run calibration changes with sudo')
    if args.operation == 'import':
        return import_bundle(root,args.bundle,args.dry_run,run)
    if args.operation == 'rollback':
        return rollback(root,args.namespace,run)
    return preview(root,args.enable,run)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('command',choices=['recover'])
    parser.add_argument('--startup',action='store_true')
    args = parser.parse_args()
    try:
        print(json.dumps(recover(startup=args.startup)))
    except (ValueError,RuntimeError,OSError) as error:
        parser.exit(2,str(error)+'\n')
