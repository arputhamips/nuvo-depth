"""Real systemd/Docker regression for the installed native runtime lifecycle.

Run as root on a Linux host with systemd and Docker (no camera hardware):
  python3 tests/native_runtime_directory_test.py --image nuvo-depth-ros:ci
The unique scratch units copy the lifecycle directives from both shipped native
profiles. The same nonroot container and read-only directory mount survive
restart, explicit stop/start, and automatic recovery after SIGKILL.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ("nuvo-depthd-native.service", "nuvo-depth-minimal.service")
DEFAULT_IMAGE = 'ros:jazzy-ros-base@sha256:103b6c417973394659af6804d4917cd489b91e5ed22eb7bef263025a3bed2410'
FIXTURE = r'''
import os, pathlib, socket, sys, uuid
root = pathlib.Path(os.environ['RUNTIME_DIRECTORY'])
if '--trigger-stop' in sys.argv:
    raise SystemExit(1 if (root / 'fail-trigger-stop').exists() else 0)
if (root / 'fail-startup').exists():
    raise SystemExit(12)
run = uuid.uuid4().hex
server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
server.bind(str(root / 'data.sock'))
os.chmod(root / 'data.sock', 0o660)
(root / 'status.json').write_text(run)
server.listen()
while True:
    connection, _ = server.accept()
    try: connection.send(run.encode())
    except OSError: pass
    finally: connection.close()
'''
PROBE = r'''
import pathlib, socket, time
root = pathlib.Path('/run/nuvo-depth')
try:
    (root / 'container-write').write_text('forbidden')
    raise RuntimeError('runtime mount unexpectedly writable')
except OSError: pass
print('READ_ONLY_VERIFIED', flush=True)
previous = None
while True:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
            client.connect(str(root / 'data.sock'))
            run = client.recv(128).decode()
            if run != previous:
                print(run, flush=True)
                previous = run
    except OSError: pass
    time.sleep(.05)
'''


def command(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True,
                          timeout=30, **kwargs).stdout.strip()


def wait(predicate, description, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError(description)


def lifecycle(profile, runtime, fixture_path):
    lines = (ROOT / "deployment/systemd" / profile).read_text().splitlines()
    preserve = [line for line in lines if line.startswith('RuntimeDirectoryPreserve=')]
    mode = [line for line in lines if line.startswith('RuntimeDirectoryMode=')]
    assert preserve == ['RuntimeDirectoryPreserve=yes'], (profile, preserve)
    assert mode == ['RuntimeDirectoryMode=0750'], (profile, mode)
    result = mode + preserve
    for phase in ('ExecStartPre=',):
        cleanup = [line for line in lines if line.startswith(phase + '/usr/bin/rm -f -- /run/nuvo-depth/')]
        assert any('/run/nuvo-depth/data.sock /run/nuvo-depth/status.json' in line for line in cleanup)
        result.extend(line.replace('/run/nuvo-depth/', str(runtime) + '/') for line in cleanup)
    # Preserve the shipped stop-command ordering, including the helper that can
    # fail. Replace its hardware operation only; do not reorder cleanup in tests.
    stop_commands = [line for line in lines if line.startswith('ExecStopPost=')]
    assert stop_commands[-1] == 'ExecStopPost=+/usr/lib/nuvo-depth/nuvo-trigger-prepare --stop'
    for line in stop_commands:
        if line.endswith('/nuvo-trigger-prepare --stop'):
            result.append('ExecStopPost=/usr/bin/python3 ' + str(fixture_path) + ' --trigger-stop')
        else:
            assert line.startswith('ExecStopPost=/usr/bin/rm -f -- /run/nuvo-depth/')
            result.append(line.replace('/run/nuvo-depth/', str(runtime) + '/'))
    return result


def exercise(profile, image, output):
    name = 'nuvo-runtime-contract-' + str(os.getpid()) + '-' + profile.split('.')[0]
    runtime = Path('/run') / name
    unit = name + '.service'
    unit_path = Path('/run/systemd/system') / unit
    fixture_path = Path('/run') / (name + '.py')
    # The test owns only these exact unique scratch paths.
    assert not runtime.exists() and not unit_path.exists() and not fixture_path.exists()
    directives = lifecycle(profile, runtime, fixture_path)
    report = dict(profile=profile, passed=False, image=image)
    container = False
    try:
        fixture_path.write_text(FIXTURE)
        fixture_path.chmod(0o644)
        unit_path.write_text('\n'.join([
            '[Unit]', 'Description=Nuvo runtime directory regression fixture',
            '[Service]', 'Type=simple', 'User=nobody', 'Group=nogroup',
            'RuntimeDirectory=' + name, *directives,
            'ExecStart=/usr/bin/python3 ' + str(fixture_path),
            'Restart=on-failure', 'RestartSec=2s', 'UMask=0027', '']))
        command('systemctl', 'daemon-reload')
        command('systemctl', 'start', unit)
        wait(lambda: (runtime / 'data.sock').exists(), 'fixture socket did not start')
        directory_inode = runtime.stat().st_ino
        assert stat.S_IMODE(runtime.stat().st_mode) == 0o750
        assert (runtime.stat().st_uid, runtime.stat().st_gid) == (65534, 65534)
        (runtime / 'unrelated-component').write_text('must survive')
        (runtime / 'calibration/requests').mkdir(parents=True)
        pending = runtime / 'calibration/requests/pending.request'
        pending.write_text('caller-owned reservation')
        (runtime / 'snapshots').mkdir()
        os.chown(runtime / 'snapshots', 65534, 65534)
        snapshot = runtime / 'snapshots/latest.nuv'
        snapshot.write_text('old producer')
        uid = int(command('docker', 'run', '--rm', '--user', '10001:10001', image, 'id', '-u'))
        assert uid not in (0, 65534), 'probe must be a distinct nonroot consumer'
        command('docker', 'run', '-d', '--rm', '--name', name,
                '--user', '10001:10001',
                '--network', 'none', '--ipc', 'private', '--group-add', '65534',
                '-v', str(runtime) + ':/run/nuvo-depth:ro',
                '--entrypoint', 'python3', image, '-u', '-c', PROBE)
        container = True

        def runs():
            logs = command('docker', 'logs', name).splitlines()
            # docker run returns before the probe necessarily writes its first
            # log line. The surrounding bounded wait must observe readiness.
            if 'READ_ONLY_VERIFIED' not in logs:
                return []
            return [line for line in logs if len(line) == 32]

        wait(lambda: len(runs()) >= 1, 'container could not reach initial native socket')
        command('systemctl', 'restart', unit)
        wait(lambda: len(runs()) >= 2, 'existing mount did not observe systemctl restart')
        assert runtime.stat().st_ino == directory_inode
        assert not snapshot.exists(), 'stale transient frame survived restart'
        shutdown_started = time.monotonic()
        command('systemctl', 'stop', unit)
        shutdown_seconds = time.monotonic() - shutdown_started
        assert shutdown_seconds < 5, 'scratch service shutdown was not bounded'
        assert runtime.is_dir() and runtime.stat().st_ino == directory_inode
        assert not (runtime / 'data.sock').exists() and not (runtime / 'status.json').exists()
        assert (runtime / 'unrelated-component').read_text() == 'must survive'
        assert pending.read_text() == 'caller-owned reservation'
        command('systemctl', 'start', unit)
        wait(lambda: len(runs()) >= 3, 'existing mount did not observe explicit stop/start')
        command('systemctl', 'kill', '--kill-whom=main', '--signal=SIGKILL', unit)
        wait(lambda: len(runs()) >= 4, 'existing mount did not observe on-failure restart')
        command('systemctl', 'stop', unit)
        (runtime / 'fail-startup').touch()
        (runtime / 'status.json').write_text('stale status before failed startup')
        command('systemctl', 'start', unit)
        wait(lambda: command('systemctl', 'show', '-p', 'SubState', '--value', unit) == 'auto-restart',
             'fixture startup failure was not observed')
        assert not (runtime / 'data.sock').exists() and not (runtime / 'status.json').exists()
        (runtime / 'fail-startup').unlink()
        wait(lambda: len(runs()) >= 5, 'existing mount did not recover after failed startup')
        (runtime / 'fail-trigger-stop').touch()
        failed_stop = subprocess.run(['systemctl', 'stop', unit], text=True,
                                     capture_output=True, timeout=30)
        # systemctl stop can return success for a completed stop job even when
        # ExecStopPost failed; the retained service Result and journal carry it.
        trigger_result = command('systemctl', 'show', '-p', 'Result', '--value', unit)
        assert trigger_result == 'exit-code', 'trigger helper failure was swallowed'
        assert not (runtime / 'data.sock').exists() and not (runtime / 'status.json').exists()
        assert runtime.stat().st_ino == directory_inode
        assert pending.exists() and (runtime / 'unrelated-component').exists()
        (runtime / 'fail-trigger-stop').unlink()
        command('systemctl', 'reset-failed', unit)
        command('systemctl', 'start', unit)
        wait(lambda: len(runs()) >= 6, 'existing mount did not recover after trigger helper failure')
        assert runtime.stat().st_ino == directory_inode
        identities = runs()
        assert len(set(identities)) >= 6, identities
        report.update(passed=True, runtime_uid=uid, runtime_gid=65534,
                      directory_inode=directory_inode, observed_run_ids=identities,
                      stop_cleanup=True, failed_start_cleanup=True,
                      trigger_failure_cleanup=True,
                      trigger_failure_result=trigger_result,
                      shutdown_seconds=shutdown_seconds, unrelated_file_preserved=True)
    finally:
        subprocess.run(['systemctl', 'stop', unit], capture_output=True, timeout=30)
        if container:
            subprocess.run(['docker', 'rm', '-f', name], capture_output=True, timeout=20)
        report['journal'] = subprocess.run(
            ['journalctl', '-u', unit, '--no-pager', '-n', '60'], text=True,
            capture_output=True, timeout=15).stdout
        unit_path.unlink(missing_ok=True)
        fixture_path.unlink(missing_ok=True)
        command('systemctl', 'daemon-reload')
        subprocess.run(['systemctl', 'reset-failed', unit], capture_output=True, timeout=15)
        if runtime.exists():
            assert runtime.parent == Path('/run') and runtime.name == name
            shutil.rmtree(runtime)
        (output / (profile + '.json')).write_text(json.dumps(report, indent=2) + '\n')
    print(profile + ': actual systemd restart, stop/start and failure recovery passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default=os.environ.get('NUVO_TEST_IMAGE', DEFAULT_IMAGE))
    parser.add_argument('--output', type=Path, default=Path('output/native-runtime-lifecycle'))
    args = parser.parse_args()
    assert os.geteuid() == 0, 'scratch systemd units require root on the test host'
    assert Path('/run/systemd/system').is_dir(), 'a running systemd host is required'
    args.output.mkdir(parents=True, exist_ok=True)
    for profile in PROFILES:
        exercise(profile, args.image, args.output)


if __name__ == '__main__':
    main()
