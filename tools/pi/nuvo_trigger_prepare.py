#!/usr/bin/env python3
"""Provision only the configured RP1 PIO PWM trigger before native capture."""
import grp
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

OWNED_LINK = Path('/run/nuvo-depth-trigger/pwm')
CONFIG_DUMP_TIMEOUT_SECONDS = 5


@contextmanager
def stage(name):
    """Leave a flushed timestamp before each operation that can stall at boot."""
    started = time.monotonic()
    print(f'nuvo-trigger-prepare stage={name} event=start monotonic_s={started:.6f}',
          file=sys.stderr, flush=True)
    try:
        yield
    except BaseException as error:
        finished = time.monotonic()
        detail = ' '.join(str(error).split())[:512]
        print(f'nuvo-trigger-prepare stage={name} event=error monotonic_s={finished:.6f} '
              f'elapsed_s={finished-started:.6f} error={type(error).__name__}: {detail}',
              file=sys.stderr, flush=True)
        raise
    else:
        finished = time.monotonic()
        print(f'nuvo-trigger-prepare stage={name} event=done monotonic_s={finished:.6f} '
              f'elapsed_s={finished-started:.6f}', file=sys.stderr, flush=True)


def configuration(config_tool, path):
    try:
        document = subprocess.check_output(
            [config_tool, '--dump-effective-config', path], text=True,
            stderr=subprocess.PIPE, timeout=CONFIG_DUMP_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f'Configuration dump exceeded {CONFIG_DUMP_TIMEOUT_SECONDS}s; '
            'inspect depthd configuration and host process/I/O stalls before retrying') from error
    except subprocess.CalledProcessError as error:
        detail = ' '.join((error.stderr or '').split())[:512]
        raise RuntimeError(f'Configuration dump failed (exit {error.returncode}): '
                           + (detail or 'inspect the depthd configuration')) from error
    return json.loads(document)['configuration']


def disable_owned_channel(link=OWNED_LINK, root=Path('/sys/class/pwm')):
    if not link.is_symlink():
        if link.exists():
            raise RuntimeError('unexpected non-symlink at trigger channel path')
        return
    if link.lstat().st_uid != 0:
        raise RuntimeError('trigger channel link must be owned by root')
    target = link.resolve()
    if not target.exists():
        link.unlink()
        return
    # Verify the retained channel by its controller, not a caller supplied path.
    for chip in root.glob('pwmchip*'):
        node = chip / 'device/of_node'
        try:
            compatible = (node / 'compatible').read_bytes().split(b'\0')
        except FileNotFoundError:
            continue
        if b'raspberrypi,pwm-pio-rp1' in compatible and (chip / 'pwm0').resolve() == target:
            (target / 'enable').write_text('0')
            return
    raise RuntimeError('owned trigger channel no longer belongs to RP1 PIO PWM')


def find_channel(line, root=Path('/sys/class/pwm')):
    for chip in root.glob('pwmchip*'):
        node = chip / 'device/of_node'
        try:
            compatible = (node / 'compatible').read_bytes().split(b'\0')
            gpios = (node / 'gpios').read_bytes()
        except FileNotFoundError:
            continue
        if b'raspberrypi,pwm-pio-rp1' in compatible and len(gpios) == 12 and struct.unpack('>III', gpios)[1] == line:
            return chip
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('configuration', nargs='?')
    parser.add_argument('--config-tool', default='/usr/lib/nuvo-depth/depthd-config')
    parser.add_argument('--stop', action='store_true')
    arguments = parser.parse_args()
    if arguments.stop:
        with stage('stop_trigger'):
            disable_owned_channel()
        return
    if not arguments.configuration:
        parser.error('configuration is required when preparing the trigger')
    with stage('config_dump'):
        config = configuration(arguments.config_tool, arguments.configuration)
    trigger = config['capture']['hardware_trigger']
    # Cleanup precedes the enabled/board checks so disabled capture or a GPIO
    # change cannot leave the previous daemon's waveform running.
    with stage('cleanup_previous_channel'):
        disable_owned_channel()
    if not trigger['enabled']:
        return
    with stage('board_check'):
        compatible = Path('/proc/device-tree/compatible')
        if not compatible.exists() or b'brcm,bcm2712' not in compatible.read_bytes().split(b'\0'):
            return
    line = trigger['line']
    if not 0 <= line <= 27 or trigger['gpiochip'] != '/dev/gpiochip0':
        raise RuntimeError('RP1 PIO trigger requires RP1 GPIO 0..27 on gpiochip0')
    with stage('find_channel'):
        chip = find_channel(line)
    if chip is None:
        with stage('load_overlay'):
            subprocess.run(['/usr/bin/dtoverlay', 'pwm-pio', f'gpio={line}'], check=True, timeout=10)
        with stage('wait_for_channel'):
            for _ in range(100):
                chip = find_channel(line)
                if chip is not None:
                    break
                time.sleep(0.02)
            if chip is None:
                raise RuntimeError('RP1 PIO PWM driver did not register the configured trigger GPIO')
    channel = chip / 'pwm0'
    with stage('export_channel'):
        if not channel.exists():
            (chip / 'export').write_text('0')
        for _ in range(100):
            if (channel / 'polarity').exists():
                break
            time.sleep(0.02)
        if not (channel / 'polarity').exists():
            raise RuntimeError('RP1 PIO PWM channel attributes did not appear after export')
    with stage('channel_permissions'):
        gid = grp.getgrnam('nuvo-video').gr_gid
        for name in ('period', 'duty_cycle', 'polarity', 'enable'):
            path = channel / name
            os.chown(path, 0, gid)
            os.chmod(path, 0o660)
    # Root-owned symlink gives systemd one specific resolved sysfs channel to
    # permit under ProtectKernelTunables, rather than exposing all of /sys.
    with stage('publish_channel'):
        directory = Path('/run/nuvo-depth-trigger')
        directory.mkdir(mode=0o755, exist_ok=True)
        link = OWNED_LINK
        if link.is_symlink():
            link.unlink()
        elif link.exists():
            raise RuntimeError('unexpected non-symlink at trigger channel path')
        link.symlink_to(channel.resolve(), target_is_directory=True)
    print(f'RP1 PIO trigger prepared on GPIO{line}: {channel.resolve()}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'nuvo-trigger-prepare: {error}', file=sys.stderr, flush=True)
        raise SystemExit(1)
