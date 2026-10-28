"""Read-only same-device ROS container discovery; never starts capture."""
from __future__ import annotations

import json
from pathlib import Path
import stat
import subprocess

import nuvo_calibration_control as calibration

CONTRACT = Path('usr/share/nuvo-depth/installation/ros-local.json')
RUNTIME = Path('run/nuvo-depth')


def add_command(commands):
    ros = commands.add_parser('ros', help='Read-only same-device ROS connection information')
    operations = ros.add_subparsers(dest='operation', required=True)
    environment = operations.add_parser('environment', help='Discover runtime paths and numeric access GID')
    environment.add_argument('--json', action='store_true', help='Print machine-readable JSON (the default)')


def group_id(root):
    if root == Path('/'):
        import grp
        try:
            return grp.getgrnam('nuvo-video').gr_gid
        except KeyError:
            return None
    try:
        for line in (root/'etc/group').read_text().splitlines():
            fields = line.split(':')
            if len(fields) == 4 and fields[0] == 'nuvo-video':
                return int(fields[2])
    except FileNotFoundError:
        pass
    return None


def environment(root=Path('/')):
    """Expose package capabilities separately from camera/capture readiness.

    An installed control broker must not imply that a deliberately disabled
    capture policy should be enabled. Discovery never connects to either socket
    or requests a frame. Shared status validation checks service activity and
    rejects stale status, without treating this as fresh-frame verification.
    """
    root = Path(root)
    try:
        path = root/CONTRACT
        contract = json.loads(path.read_bytes()) if path.stat().st_size <= 65536 else None
    except (OSError, ValueError):
        contract = None
    supported = isinstance(contract, dict) and contract.get('schema_version') == 1
    gid = group_id(root)
    checks = {}
    for name, relative, socket in (('runtime_directory', RUNTIME, False),
                                   ('data_socket', RUNTIME/'data.sock', True),
                                   ('control_socket', RUNTIME/'calibration.sock', True)):
        try:
            info = (root/relative).stat()
            kind = stat.S_ISSOCK(info.st_mode) if socket else stat.S_ISDIR(info.st_mode)
            # The launcher adds exactly this numeric host GID to its container.
            needed = 0o060 if socket else 0o050
            accessible = kind and gid is not None and info.st_gid == gid and info.st_mode & needed == needed
        except OSError:
            kind = accessible = False
        checks[name] = {'present': kind, 'group_access': accessible}
    try:
        capture_enabled = calibration.service_configuration(root)['capture_policy_enabled']
        policy_known = True
    except (OSError, ValueError, subprocess.SubprocessError):
        capture_enabled = False
        policy_known = False
    health = calibration.daemon_health(root)
    reasons = []
    if not supported or contract.get('matching_grid_geometry') is not True:
        reasons.append('native_package_upgrade_required')
    if gid is None:
        reasons.append('nuvo_video_group_missing')
    if not checks['runtime_directory']['group_access']:
        reasons.append('runtime_directory_unavailable')
    if not policy_known:
        reasons.append('capture_policy_unavailable')
    elif not capture_enabled:
        reasons.append('capture_disabled')
    if not checks['data_socket']['group_access']:
        reasons.append('data_socket_unavailable')
    if health.get('status') != 'running':
        reasons.append('daemon_not_running')
    return {'schema_version': 1, 'transport': 'local',
            'runtime_directory': '/run/nuvo-depth',
            'data_socket': '/run/nuvo-depth/data.sock',
            'control_socket': '/run/nuvo-depth/calibration.sock',
            'access_group': 'nuvo-video', 'access_gid': gid,
            'matching_grid_geometry': supported and contract.get('matching_grid_geometry') is True,
            'calibration_control': contract.get('calibration_control') if supported else None,
            'capture_enabled': capture_enabled, 'daemon_status': health.get('status'),
            'data_ready': not reasons,
            'control_ready': supported and contract.get('calibration_control') == 'explicit_bundle_import_v1'
                             and checks['runtime_directory']['group_access'] and checks['control_socket']['group_access'],
            'checks': checks, 'reasons': reasons}


def print_install_hint(root=Path('/')):
    value = environment(root)
    gid = value['access_gid']
    print('Same-device ROS: native runtime and nuvo-video access are prepared. '
          'The ROS container launcher discovers this host\'s numeric GID automatically '
          f'({gid if gid is not None else "unavailable"}). Inspect sudo nuvoctl ros environment --json.')
    if not value['capture_enabled']:
        print('Capture remains disabled by the existing/requested policy; connecting ROS does not enable it.')
