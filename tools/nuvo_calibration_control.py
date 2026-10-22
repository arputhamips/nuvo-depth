"""Shared calibration readiness and device control, without image-solving imports."""
from __future__ import annotations

import json
from pathlib import Path
import re
import shlex
import subprocess
import time

from nuvo_calibration_store import validate_bundle

STATE = Path('var/lib/nuvo-depth')
CONFIG = Path('etc/nuvo-depth/daemon.yaml')
MARKER = Path('etc/nuvo-depth/capture-enabled')
SERVICE = 'nuvo-depthd-native.service'


def read_json(path, limit=65536):
    if not path.is_file() or path.stat().st_size > limit:
        raise ValueError(f'Missing or oversized status/configuration: {path}')
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError(f'Expected a JSON object: {path}')
    return value


def daemon_health(root):
    path = root / 'run/nuvo-depth/status.json'
    try:
        health = read_json(path)
        if time.time() - path.stat().st_mtime > 15:
            raise ValueError('Daemon status is stale')
        return health
    except (OSError, ValueError) as error:
        return {'status': 'unavailable', 'error': str(error)}


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
    health = daemon_health(root)
    result = {'schema_version': 1, 'state': 'unavailable', 'label': 'Status unavailable/error',
              'message': 'Calibration readiness could not be determined.',
              'next_action': 'Inspect the specific problem and repair the configuration or service.',
              'accepted': False, 'preview_enabled': False, 'capture_enabled': False,
              'bundle_id': None, 'loaded': None,
              'camera_service': {key: health.get(key) for key in
                                 ['status', 'run_uuid', 'capture_sequence', 'error']}}
    try:
        policy = service_configuration(root, run)
        result.update(preview_enabled=policy['preview_enabled'],
                      capture_enabled=policy['capture_policy_enabled'])
        cfg = configuration(root, run)
        manifest = selected_calibration(root, run, cfg)
        if manifest:
            result['bundle_id'] = manifest['bundle_id']
            if health.get('status') == 'running':
                result['loaded'] = health.get('calibration_sha256') == manifest['bundle_id']
                expected_authority = 2 if manifest['authority'] == 'factory_validated' else 1
                if not result['loaded'] or health.get('calibration_authority') != expected_authority:
                    raise ValueError('The selected accepted bundle is not the calibration loaded by the daemon')
            result.update(state='calibration_accepted', label='Calibration accepted', accepted=True,
                          message='A matching per-unit calibration passed its declared validation. This is not independent metric-accuracy or production qualification.',
                          next_action='Inspect calibration details and verify fresh output; check camera/service health separately.')
        elif health.get('status') == 'running' and health.get('calibration_authority', 0) > 0:
            raise ValueError('Existing validated calibration files are preserved, but a matching per-unit bundle cannot be verified. Import the original compatible bundle; a software upgrade alone does not require recalibration.')
        elif policy['preview_enabled'] and policy['capture_policy_enabled']:
            result.update(state='preview_enabled', label='Preview enabled',
                          message='Provisional output is enabled; distance accuracy remains unverified. Camera/service health is separate.',
                          next_action='Continue engineering evaluation or import an accepted calibration bundle.')
        else:
            result.update(state='calibration_needed', label='Calibration needed', capture_enabled=False,
                          message='Capture is waiting for calibration or an explicit provisional-preview selection.',
                          next_action='Import a compatible accepted bundle, or deliberately enable provisional preview.')
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        result['problem'] = str(error)
        result['message'] = str(error)
    return result


def status(root=Path('/'), run=subprocess.run):
    root = Path(root)
    return {**daemon_health(root), 'calibration_readiness': readiness(root, run)}
