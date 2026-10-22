"""Obtain the first useful depth sample, or inspect a browser .nuv download."""
from __future__ import annotations

import argparse
import getpass
import math
import os
from pathlib import Path
import struct
import time

from nuvo_depth import NuvoClient, RecordingReader, RecordingWriter, decode_frame


def describe(frame):
    depth, validity = frame.products.get('depth'), frame.products.get('validity')
    if (depth is None or validity is None or depth.format != 'float32_le'
            or validity.format != 'uint8' or (depth.width, depth.height) != (validity.width, validity.height)):
        raise ValueError('Sample needs associated depth and validity with matching dimensions')
    count, selected = 0, None
    for index, (value,) in enumerate(struct.iter_unpack('<f', depth.data)):
        if validity.data[index] == 1 and math.isfinite(value) and value > 0:
            count += 1
            if selected is None:
                selected = (index, value)
    authority = {0: 'Preview: metric accuracy unverified', 1: 'User-validated calibration',
                 2: 'Factory-validated calibration'}.get(frame.calibration_authority, 'Unknown calibration authority')
    lines = [f'Frame: {frame.identity}', f'Dimensions: {depth.width} x {depth.height}',
             f'Calibration: {authority}', f'Calibration SHA-256: {frame.calibration_sha256}',
             f'Valid depth pixels: {count} / {depth.width * depth.height}']
    if selected is None:
        raise ValueError('No valid depth pixels. Check a textured, well-lit scene, exposure and stereo alignment.')
    index, value = selected
    lines.append(f'Sample at x={index % depth.width}, y={index // depth.width}: {value:.4f} m optical-axis Z')
    return '\n'.join(lines)


def capture(client, output, *, deadline_seconds=20):
    deadline = time.monotonic() + deadline_seconds
    previous = None
    with client.frames(products=('depth', 'validity'), cadence_hz=1, delivery='best_effort') as frames:
        for frame in frames:
            if time.monotonic() > deadline:
                raise TimeoutError('No advancing depth frames within the first-use deadline')
            # Validate products and numerical usefulness before claiming success.
            report = describe(frame)
            if (previous is not None and frame.identity[:3] == previous.identity[:3]
                    and frame.capture_sequence > previous.capture_sequence
                    and frame.capture_time_ns > previous.capture_time_ns):
                with RecordingWriter(output) as writer:
                    writer.write(frame)
                return report
            if previous is None or frame.identity[:3] != previous.identity[:3]:
                previous = frame
    raise RuntimeError('Stream ended before two advancing depth frames arrived')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url', nargs='?', help='HTTPS URL with the certificate hostname')
    parser.add_argument('--ca-file', type=Path, help='Verified device certificate')
    parser.add_argument('--token-file', type=Path, help='Private administrator token file; otherwise prompt without echo')
    parser.add_argument('--output', type=Path, default=Path('first-depth.nvr'))
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--input-nuv', type=Path, help='Inspect a saved browser NUV1 frame offline')
    source.add_argument('--replay', type=Path, help='Inspect an NVR1 recording offline')
    args = parser.parse_args(argv)
    try:
        if args.input_nuv or args.replay:
            if args.url or args.token_file:
                parser.error('offline inspection does not take a URL or token')
            if args.input_nuv:
                if args.input_nuv.stat().st_size > 64 * 1024**2 + 65568:
                    raise ValueError('Sample exceeds wire size bounds')
                print(describe(decode_frame(args.input_nuv.read_bytes())))
            else:
                with RecordingReader(args.replay) as reader:
                    for frame in reader:
                        print(describe(frame))
                        break
                    else:
                        raise ValueError('Recording contains no frames')
            print('Saved sample inspected; live freshness was not checked.')
            return
        if not args.url or not args.ca_file:
            parser.error('live capture needs URL and --ca-file')
        if args.output.exists():
            raise FileExistsError(f'{args.output} already exists; choose a new --output file')
        client = NuvoClient(args.url, ca_file=str(args.ca_file), timeout=10)
        token = args.token_file.read_text().strip() if args.token_file else os.environ.get('NUVO_DEPTH_TOKEN') or getpass.getpass('Device administrator token (hidden): ')
        client.claim(token)
        print(capture(client, args.output))
        print(f'Fresh depth confirmed. Saved {args.output} (NVR1; depth, validity and original metadata).')
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f'First depth: {error}\nCheck capture/calibration status and Raw product delivery in the console.\n')


if __name__ == '__main__':
    main()
