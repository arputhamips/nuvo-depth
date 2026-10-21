#!/usr/bin/env python3
"""Physical-camera acceptance for an installed minimal Actions package.

Run as root on an otherwise idle Pi 5. Changes exposure/stereo controls and
restarts the minimal service; restores both original documents in finally.
Only Python's standard library and the installed CLI are required.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import time

HEADER = struct.Struct('>4sHHHHIQQ')
SERVICE = 'nuvo-depthd-native.service'


def command(*args):
    return subprocess.check_output(list(args), text=True)


def status():
    return json.loads(command('nuvoctl', 'status', '--json'))


def capture(directory, label):
    path = directory/(label+'.f32')
    subprocess.run(['nuvoctl', 'depth', 'capture', '--timeout-seconds', '30', '--output', str(path)], check=True)
    metadata = json.loads(Path(str(path)+'.json').read_text())
    dimensions = next(p for p in metadata['products'] if p['kind'] == 'depth')
    pixels = dimensions['width']*dimensions['height']
    depth = path.read_bytes(); validity = Path(str(path)+'.validity.u8').read_bytes()
    assert len(depth) == pixels*4 and len(validity) == pixels
    assert metadata['depth_unit'] == 'metres'
    finite = 0
    for index, (value,) in enumerate(struct.iter_unpack('<f', depth)):
        if validity[index] == 1:
            assert math.isfinite(value) and value > 0
            finite += 1
        else:
            assert math.isnan(value)
    assert finite > 0, 'capture contains no valid depth pixels'
    assert {p['kind'] for p in metadata['products']} == {'depth', 'validity'}
    return {'metadata': metadata, 'valid_pixels': finite, 'pixels': pixels,
            'depth_sha256': hashlib.sha256(depth).hexdigest()}


def assert_health(document):
    for key in ('trigger_errors', 'request_requeue_failures', 'scheduler_processing_failures'):
        assert key in document and document[key] == 0, (key, document.get(key))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=60)
    args = parser.parse_args()
    assert 10 <= args.frames <= 300
    controls = [Path('/etc/nuvo-depth')/name for name in ('exposure.json', 'stereo.json')]
    originals = {path: path.read_bytes() for path in controls}
    report = {'schema_version': 1, 'test': 'minimal-raw-depth-physical-pi',
              'controls': {path.name: json.loads(data) for path, data in originals.items()},
              'kernel': command('uname', '-r').strip(),
              'package': command('dpkg-query', '-W', '-f=${Version}', 'nuvo-depth-minimal').strip()}
    try:
        for package in ('nuvo-depth', 'nuvo-depth-gateway', 'nuvo-depth-tools', 'nuvo-depth-calibration',
                        'python3-nuvo-depth', 'network-manager', 'dnsmasq-base', 'avahi-daemon',
                        'python3-opencv', 'python3-numpy', 'ffmpeg'):
            installed = subprocess.run(['dpkg-query', '-W', '-f=${db:Status-Status}', package], capture_output=True, text=True)
            assert installed.stdout.strip() != 'installed', package
        report['excluded_packages_absent'] = True
        command('nuvoctl', 'config', 'validate')
        with tempfile.TemporaryDirectory(prefix='nuvo-minimal-hil-') as temporary:
            directory = Path(temporary)
            report['single_capture'] = capture(directory, 'initial')
            stream = directory/'stream.nuv'
            started = time.monotonic()
            subprocess.run(['nuvoctl', 'depth', 'stream', '--frames', str(args.frames), '--hz', '10',
                            '--timeout-seconds', '30', '--output', str(stream)], check=True)
            elapsed = time.monotonic()-started
            identities = []; timestamps = []; offset = 0
            data = stream.read_bytes()
            while offset < len(data):
                magic, major, minor, kind, flags, metadata_size, payload_size, sequence = HEADER.unpack_from(data, offset)
                assert (magic, major, minor, kind, flags) == (b'NUV1', 1, 0, 5, 0)
                metadata = json.loads(data[offset+HEADER.size:offset+HEADER.size+metadata_size])
                assert {p['kind'] for p in metadata['products']} == {'depth', 'validity'}
                identities.append((metadata['run_uuid'], metadata['capture_epoch'], metadata['capture_sequence']))
                timestamps.append(metadata['capture_time_ns'])
                offset += HEADER.size+metadata_size+payload_size
            assert offset == len(data) and len(identities) == args.frames
            assert len(set(identities)) == args.frames
            assert all(a < b for a, b in zip(timestamps, timestamps[1:]))
            age = time.clock_gettime(time.CLOCK_BOOTTIME)-timestamps[-1]/1e9
            assert 0 <= age < 10, age
            report['stream'] = {'frames': len(identities), 'wall_seconds': elapsed,
                                'capture_rate_hz': (len(timestamps)-1)*1e9/(timestamps[-1]-timestamps[0]),
                                'last_frame_age_seconds': age, 'bytes': len(data)}
            report['active_status'] = status(); assert_health(report['active_status'])
            command('nuvoctl', 'config', 'set', 'exposure.manual_exposure_us', '6000')
            deadline = time.monotonic()+5
            while status().get('requested_exposure_us') != 6000 and time.monotonic() < deadline: time.sleep(0.1)
            assert status()['requested_exposure_us'] == 6000
            command('nuvoctl', 'config', 'set', 'stereo.uniqueness_ratio', '1')
            changed = capture(directory, 'changed')
            expected_revision = hashlib.sha256(Path('/etc/nuvo-depth/stereo.json').read_bytes()).hexdigest()
            deadline = time.monotonic()+5
            while status().get('stereo_applied_revision') != expected_revision and time.monotonic() < deadline: time.sleep(0.1)
            assert status()['stereo_applied_revision'] == expected_revision
            report['live_configuration'] = {'exposure_us': status()['requested_exposure_us'],
                                            'stereo_revision': status()['stereo_applied_revision']}
            report['restart_captures'] = []
            previous = changed['metadata']['run_uuid']
            for cycle in range(3):
                subprocess.run(['nuvoctl', 'restart'], check=True)
                observed = capture(directory, f'restart-{cycle}')
                assert observed['metadata']['run_uuid'] != previous
                previous = observed['metadata']['run_uuid']
                assert_health(status())
                report['restart_captures'].append(observed)
            subprocess.run(['nuvoctl', 'stop'], check=True)
            pwm = Path('/run/nuvo-depth-trigger/pwm')
            assert pwm.is_symlink() and (pwm/'enable').read_text().strip() == '0'
            report['trigger_disabled_after_stop'] = True
            subprocess.run(['nuvoctl', 'start'], check=True)
            capture(directory, 'after-stop')
            time.sleep(1)
            idle_before = status(); time.sleep(1); idle_after = status()
            assert idle_after['raw_product_demand'] == 0
            assert not idle_after['depth_conversion_active']
            assert idle_after['matched_frames'] == idle_before['matched_frames']
            assert idle_after['captured_pairs'] > idle_before['captured_pairs']
            report['idle_processing_stops'] = True
            report['final_status'] = idle_after
            temperature = Path('/sys/class/thermal/thermal_zone0/temp')
            if temperature.exists(): report['temperature_c'] = int(temperature.read_text())/1000
        report['passed'] = True
    finally:
        for path, original in originals.items():
            temporary = path.with_suffix('.hil-restore')
            temporary.write_bytes(original); temporary.chmod(0o644); temporary.replace(path)
        subprocess.run(['nuvoctl', 'restart'], check=True)
        report['controls_restored'] = all(path.read_bytes() == original for path, original in originals.items())
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'passed': report['passed'], 'report': str(args.report), 'stream': report['stream']}))


if __name__ == '__main__': main()
