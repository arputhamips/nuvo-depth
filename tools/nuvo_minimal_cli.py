#!/usr/bin/env python3
"""SSH-oriented configuration and raw depth output for the minimal Pi package."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'lib/nuvo-depth'))
import nuvoctl as admin

SERVICE = 'nuvo-depthd-native.service'
HEADER = struct.Struct('>4sHHHHIQQ')
CONFLICTS = ('nuvo-depth', 'nuvo-depth-core', 'nuvo-depth-platform-pi5', 'nuvo-depth-tools', 'nuvo-depthd')


def configuration(root: Path) -> dict:
    return {name: json.loads((root/'etc/nuvo-depth'/f'{name}.json').read_text())
            for name in ('stereo', 'exposure')}


def set_value(root: Path, key: str, text: str) -> dict:
    section, separator, field = key.partition('.')
    if not separator or section not in ('stereo', 'exposure'):
        raise ValueError('use a stereo.FIELD or exposure.FIELD shown by config show')
    path = root/'etc/nuvo-depth'/f'{section}.json'
    original = path.read_bytes()
    current = json.loads(original)
    if field == 'schema_version' or field not in current:
        raise ValueError('use a stereo.FIELD or exposure.FIELD shown by config show')
    revision = '"' + hashlib.sha256(original).hexdigest() + '"'
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        value = text
    current[field] = value
    setter = admin.set_stereo if section == 'stereo' else admin.set_exposure
    return setter(root, current, revision)


def configure(root: Path, input_fn=input) -> dict:
    original = {name: (root/'etc/nuvo-depth'/f'{name}.json').read_bytes() for name in ('stereo', 'exposure')}
    current = {name: json.loads(data) for name, data in original.items()}
    prompts = (('exposure', 'mode', 'Exposure mode (manual/auto)'),
               ('exposure', 'manual_exposure_us', 'Manual exposure, microseconds'),
               ('exposure', 'manual_analogue_gain', 'Manual analogue gain'),
               ('stereo', 'algorithm', 'Stereo algorithm (sgbm/bm)'),
               ('stereo', 'search_range', 'Disparity search range (multiple of 16)'),
               ('stereo', 'max_disparity', 'Maximum disparity'),
               ('stereo', 'block_size', 'Odd block size'),
               ('stereo', 'opencv_threads', 'OpenCV threads (1..4)'))
    for section, key, label in prompts:
        answer = input_fn(f'{label} [{current[section][key]}]: ').strip()
        if answer:
            try: current[section][key] = json.loads(answer)
            except json.JSONDecodeError: current[section][key] = answer
    admin.validate_stereo(current['stereo'])
    admin.validate_exposure(current['exposure'])
    with admin.configuration_lock(root):
        for name, data in original.items():
            if (root/'etc/nuvo-depth'/f'{name}.json').read_bytes() != data:
                raise ValueError('configuration changed while answering; rerun configure')
        for name, document in current.items():
            admin.atomic_write(root/'etc/nuvo-depth'/f'{name}.json', json.dumps(document, sort_keys=True, separators=(',', ':'))+'\n', 0o644)
    return {'saved': current, 'application': 'The running daemon reloads these controls; inspect status for applied revisions.'}


def decode_capture(data: bytes) -> tuple[dict, bytes, bytes]:
    if len(data) < HEADER.size: raise ValueError('truncated depth frame')
    magic, major, minor, kind, flags, metadata_size, payload_size, sequence = HEADER.unpack_from(data)
    if ((magic, major, minor, kind, flags) != (b'NUV1', 1, 0, 5, 0) or not sequence
            or not 0 < metadata_size <= 65536 or not 0 < payload_size <= 64*1024**2
            or len(data) != HEADER.size + metadata_size + payload_size):
        raise ValueError('invalid bounded NuvoStream frame')
    metadata = json.loads(data[HEADER.size:HEADER.size+metadata_size])
    if metadata.get('schema_version') != 1 or metadata.get('depth_unit') != 'metres':
        raise ValueError('unsupported depth metadata or units')
    payload = memoryview(data)[HEADER.size+metadata_size:]
    products = metadata.get('products', [])
    if len(products) != 2 or {p['kind'] for p in products} != {'depth', 'validity'}:
        raise ValueError('expected only associated depth and validity')
    selected = {}
    intervals = []
    for product in products:
        name = product['kind']
        width, height, offset, length = (product[k] for k in ('width', 'height', 'offset', 'length'))
        if any(type(n) is not int for n in (width, height, offset, length)):
            raise ValueError('invalid product dimensions')
        size = 4 if name == 'depth' else 1
        if (product['format'] != ('float32_le' if name == 'depth' else 'uint8')
                or width <= 0 or height <= 0 or offset < 0 or length != width*height*size
                or offset+length > len(payload)):
            raise ValueError('invalid product bounds or encoding')
        intervals.append((offset, offset+length))
        selected[name] = (width, height, bytes(payload[offset:offset+length]))
    intervals.sort()
    if intervals[0][0] != 0 or intervals[0][1] != intervals[1][0] or intervals[1][1] != len(payload):
        raise ValueError('overlapping or incomplete product payload')
    if selected['depth'][:2] != selected['validity'][:2]:
        raise ValueError('depth and validity dimensions differ')
    return metadata, selected['depth'][2], selected['validity'][2]


def reader_command(args) -> list[str]:
    return [args.reader, '--socket', args.socket, '--frames', str(args.frames),
            '--hz', str(args.hz), '--timeout-seconds', str(args.timeout_seconds)]


def depth_output(args) -> None:
    if not math.isfinite(args.hz) or not 0.001 <= args.hz <= 60:
        raise ValueError('hz must be in 0.001..60')
    if not 1 <= args.timeout_seconds <= 120 or not 0 <= args.frames <= 1_000_000:
        raise ValueError('invalid timeout or frame count')
    if args.format == 'f32' and (args.frames != 1 or args.output == '-'):
        raise ValueError('f32 capture needs one frame and a file; use nuv for streaming')
    command = reader_command(args)
    if args.output == '-':
        subprocess.run(command, check=True)
        return
    target = Path(args.output)
    names = [target] if args.format == 'nuv' else [target, Path(str(target)+'.validity.u8'), Path(str(target)+'.json')]
    created = []
    try:
        # Reserve every output without overwriting previous recordings.
        with context_files(names, created) as files:
            if args.format == 'nuv':
                subprocess.run(command, stdout=files[0], check=True)
            else:
                result = subprocess.run(command, stdout=subprocess.PIPE, check=True)
                metadata, depth, validity = decode_capture(result.stdout)
                files[0].write(depth)
                files[1].write(validity)
                files[2].write((json.dumps(metadata, sort_keys=True, indent=2)+'\n').encode())
        print(json.dumps({'files': [str(p) for p in names], 'format': args.format}), file=sys.stderr)
    except BaseException:
        for path in created: path.unlink(missing_ok=True)
        raise


@contextmanager
def context_files(names, created):
    files = []
    try:
        for path in names:
            files.append(path.open('xb'))
            created.append(path)
        yield files
    finally:
        for output in files: output.close()


def cli_parser():
    parser = argparse.ArgumentParser(prog='nuvoctl', description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/'), help=argparse.SUPPRESS)
    commands = parser.add_subparsers(dest='command', required=True)
    calibration = commands.add_parser('calibration').add_subparsers(dest='operation', required=True)
    calibration.add_parser('status')
    commands.add_parser('configure', help='Guided exposure and stereo configuration')
    config = commands.add_parser('config').add_subparsers(dest='operation', required=True)
    config.add_parser('show')
    config.add_parser('validate')
    change = config.add_parser('set')
    change.add_argument('key'); change.add_argument('value')
    status = commands.add_parser('status'); status.add_argument('--json', action='store_true')
    for name in ('start', 'stop', 'restart'): commands.add_parser(name)
    depth = commands.add_parser('depth').add_subparsers(dest='operation', required=True)
    for operation in ('capture', 'stream'):
        output = depth.add_parser(operation)
        output.add_argument('--frames', type=int, default=1 if operation == 'capture' else 0)
        output.add_argument('--hz', type=float, default=10.0, help='Requested rate; actual throughput depends on matcher settings')
        output.add_argument('--timeout-seconds', type=int, default=10)
        output.add_argument('--format', choices=('f32', 'nuv'), default='f32' if operation == 'capture' else 'nuv')
        output.add_argument('--output', default='depth.f32' if operation == 'capture' else '-')
        output.add_argument('--socket', default='/run/nuvo-depth/data.sock')
        output.add_argument('--reader', default='/usr/lib/nuvo-depth/nuvo-depth-read', help=argparse.SUPPRESS)
    return parser


def main(argv=None):
    if (argv or sys.argv[1:])[:1] == ['install']:
        return install_main((argv or sys.argv[1:])[1:])
    args = cli_parser().parse_args(argv)
    root = args.root
    if args.command == 'depth': return depth_output(args)
    if args.command in ('start', 'stop', 'restart'):
        subprocess.run(['systemctl', args.command, SERVICE], check=True)
        return
    if args.command == 'configure': result = configure(root)
    elif args.command in ('status', 'calibration'):
        import nuvo_calibration_control as calibration_control
        result = calibration_control.status(root) if args.command == 'status' else calibration_control.readiness(root)
    elif args.operation == 'set': result = set_value(root, args.key, args.value)
    else:
        controls = configuration(root)
        admin.validate_stereo(controls['stereo']); admin.validate_exposure(controls['exposure'])
        effective = subprocess.check_output(['depthd', '--dump-effective-config', str(root/'etc/nuvo-depth/daemon.yaml')], text=True)
        result = {'controls': controls, 'daemon': json.loads(effective)}
    print(json.dumps(result, sort_keys=True, indent=2, allow_nan=False))


def install_preflight(root: Path, run=subprocess.run, *, kernel=None) -> dict:
    if not admin.device_model(root).startswith('Raspberry Pi 5'):
        raise ValueError('minimal capture requires a Raspberry Pi 5')
    values = dict(line.split('=', 1) for line in (root/'etc/os-release').read_text().splitlines() if '=' in line)
    if values.get('VERSION_CODENAME', '').strip('"') != 'trixie':
        raise ValueError('use Raspberry Pi OS Lite 64-bit Trixie')
    arch = run(['dpkg', '--print-architecture'], check=True, capture_output=True, text=True).stdout.strip()
    if arch != 'arm64': raise ValueError('arm64 userspace is required')
    conflicts = []
    for package in CONFLICTS:
        result = run(['dpkg-query', '-W', '-f=${db:Status-Status}', package], capture_output=True, text=True)
        if result.returncode == 0 and result.stdout.strip() == 'installed': conflicts.append(package)
    if conflicts: raise ValueError('minimal and full installations are exclusive; existing packages: '+', '.join(conflicts))
    if run(['dpkg', '--audit'], check=True, capture_output=True, text=True).stdout.strip():
        raise ValueError('repair the interrupted dpkg transaction before installing')
    return {'architecture': arch, 'kernel': kernel or platform.release(), 'codename': 'trixie'}


def install_main(argv=None):
    import nuvo_release as release
    parser = argparse.ArgumentParser(description='Install the minimal raw-depth Pi runtime; existing SSH/network configuration is retained.')
    parser.add_argument('--bundle', type=Path, help='Extracted signed minimal bundle')
    parser.add_argument('--package', type=Path, help='Local Actions artifact package (no release required)')
    parser.add_argument('--platform', type=Path, help='release-platform.json from the same Actions artifact')
    parser.add_argument('--repository', default=release.DEFAULT_REPOSITORY)
    parser.add_argument('--release')
    parser.add_argument('--public-key', type=Path)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--reboot', action='store_true')
    args = parser.parse_args(argv)
    if args.package and (args.bundle or not args.platform): parser.error('--package requires --platform and cannot be combined with --bundle')
    if not args.dry_run and os.geteuid() != 0: parser.error('run installation with sudo')
    with tempfile.TemporaryDirectory(prefix='nuvo-minimal-') as temporary:
        directory = Path(temporary)
        if args.package:
            package = args.package.resolve()
            fields = subprocess.check_output(['dpkg-deb', '-f', str(package), 'Package', 'Architecture', 'Version'], text=True)
            metadata = dict(line.split(': ', 1) for line in fields.strip().splitlines())
            if metadata.get('Package') != 'nuvo-depth-minimal' or metadata.get('Architecture') != 'arm64':
                raise ValueError('select the ARM64 nuvo-depth-minimal Actions artifact package')
            cohort = release.validate_platform(release.load_json(args.platform))
            packages = [package]
        else:
            key = args.public_key
            if key is None:
                key = directory/'release.pub'; key.write_text(release.DEFAULT_PUBLIC_KEY)
            manifest, packages = release.acquire_release(directory, key, args.repository, args.release, args.bundle, profile='minimal')
            if manifest.get('profile') != 'minimal': raise ValueError('select a minimal bundle')
            cohort = manifest['platform']
        state = install_preflight(Path('/'))
        if state['kernel'] not in cohort['kernels']: raise ValueError('kernel is outside the artifact qualified cohort')
        # Validate the managed boot changes before touching installed packages.
        preparation = admin.platform_prepare(Path('/'), False)
        old = subprocess.run(['dpkg-query', '-W', '-f=${Version}', 'nuvo-depth-minimal'], capture_output=True, text=True)
        version = subprocess.check_output(['dpkg-deb', '-f', str(packages[0]), 'Version'], text=True).strip()
        if old.returncode == 0 and subprocess.run(['dpkg', '--compare-versions', old.stdout.strip(), 'gt', version]).returncode == 0:
            raise ValueError('refusing an implicit package downgrade')
        command = ['apt-get', '--no-install-recommends', '--no-remove', '--reinstall', 'install', *map(str, packages)]
        print(json.dumps({'profile': 'minimal', 'platform': state, 'packages': list(map(str, packages)), 'boot': preparation}, indent=2), flush=True)
        if not args.dry_run: subprocess.run(['apt-get', 'update'], check=True)
        subprocess.run(['apt-get', '--simulate', *command[1:]], check=True)
        if args.dry_run: return
        # Serialize package/boot changes independently of the control-file lock.
        lock_path = Path('/var/lib/nuvo-depth-minimal-install.lock')
        import fcntl
        with lock_path.open('a') as install_lock:
            fcntl.flock(install_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return apply_install(command, preparation, args)


def apply_install(command, preparation, args):
    install_preflight(Path('/'))
    environment = dict(os.environ, DEBIAN_FRONTEND='noninteractive')
    subprocess.run([*command[:1], '--yes', '-o', 'Dpkg::Options::=--force-confdef', '-o', 'Dpkg::Options::=--force-confold', *command[1:]], env=environment, check=True)
    preparation = admin.platform_prepare(Path('/'), True)
    subprocess.run(['depthd', '--check-config', '/etc/nuvo-depth/daemon.yaml'], check=True)
    subprocess.run(['systemctl', 'daemon-reload'], check=True)
    subprocess.run(['systemctl', 'enable', SERVICE], check=True)
    if preparation['reboot_required'] or not admin.platform_preflight(Path('/'))['ready']:
        print('Camera boot configuration prepared. Reboot, reconnect over SSH, then run sudo nuvoctl depth capture.')
        if args.reboot: subprocess.run(['systemctl', 'reboot'], check=True)
        return
    subprocess.run(['systemctl', 'restart', SERVICE], check=True)
    # Service readiness alone does not exercise the demand-driven matcher.
    subprocess.run(['/usr/lib/nuvo-depth/nuvo-depth-read', '--frames', '3', '--timeout-seconds', '30'], stdout=subprocess.DEVNULL, check=True)
    print('Minimal installation verified: three fresh raw depth frames. Configure with sudo nuvoctl configure.')


if __name__ == '__main__':
    try: main()
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print('nuvoctl: '+str(error), file=sys.stderr)
        raise SystemExit(2)
    except KeyboardInterrupt:
        raise SystemExit(130)
