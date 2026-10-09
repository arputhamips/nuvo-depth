#!/usr/bin/env python3
"""Provision only the configured RP1 PIO PWM trigger before native capture."""
import grp
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

OWNED_LINK = Path('/run/nuvo-depth-trigger/pwm')


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
    parser.add_argument('--depthd', default='/usr/bin/depthd')
    parser.add_argument('--stop', action='store_true')
    arguments = parser.parse_args()
    if arguments.stop:
        disable_owned_channel()
        return
    if not arguments.configuration:
        parser.error('configuration is required when preparing the trigger')
    config = json.loads(subprocess.check_output(
        [arguments.depthd, '--dump-effective-config', arguments.configuration], text=True))['configuration']
    trigger = config['capture']['hardware_trigger']
    # Cleanup precedes the enabled/board checks so disabled capture or a GPIO
    # change cannot leave the previous daemon's waveform running.
    disable_owned_channel()
    if not trigger['enabled']:
        return
    compatible = Path('/proc/device-tree/compatible')
    if not compatible.exists() or b'brcm,bcm2712' not in compatible.read_bytes().split(b'\0'):
        return
    line = trigger['line']
    if not 0 <= line <= 27 or trigger['gpiochip'] != '/dev/gpiochip0':
        raise RuntimeError('RP1 PIO trigger requires RP1 GPIO 0..27 on gpiochip0')
    chip = find_channel(line)
    if chip is None:
        subprocess.run(['/usr/bin/dtoverlay', 'pwm-pio', f'gpio={line}'], check=True, timeout=10)
        for _ in range(100):
            chip = find_channel(line)
            if chip is not None:
                break
            time.sleep(0.02)
    if chip is None:
        raise RuntimeError('RP1 PIO PWM driver did not register the configured trigger GPIO')
    channel = chip / 'pwm0'
    if not channel.exists():
        (chip / 'export').write_text('0')
    for _ in range(100):
        if (channel / 'polarity').exists():
            break
        time.sleep(0.02)
    gid = grp.getgrnam('nuvo-video').gr_gid
    for name in ('period', 'duty_cycle', 'polarity', 'enable'):
        path = channel / name
        os.chown(path, 0, gid)
        os.chmod(path, 0o660)
    # Root-owned symlink gives systemd one specific resolved sysfs channel to
    # permit under ProtectKernelTunables, rather than exposing all of /sys.
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
    main()
