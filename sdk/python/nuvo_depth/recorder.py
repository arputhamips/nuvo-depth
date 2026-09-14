from __future__ import annotations

import argparse
import time

from .client import NuvoClient
from .recording import RecordingWriter


def main() -> None:
    parser = argparse.ArgumentParser(description="Record associated Nuvo frame sets")
    parser.add_argument("url")
    parser.add_argument("output")
    parser.add_argument("--claim-token", required=True)
    parser.add_argument("--ca-file")
    parser.add_argument("--frames", type=int, default=0)
    parser.add_argument("--cadence", type=float, default=10.0)
    args = parser.parse_args()
    if args.frames < 0 or not 0 < args.cadence <= 60:
        parser.error("invalid frame count or cadence")
    client = NuvoClient(args.url, ca_file=args.ca_file)
    client.claim(args.claim_token)
    count = 0
    previous = None
    with RecordingWriter(args.output) as writer:
        while args.frames == 0 or count < args.frames:
            frame = client.latest()
            identity = (frame.capture_epoch, frame.capture_sequence)
            if identity != previous:
                writer.write(frame); previous = identity; count += 1
            time.sleep(1.0 / args.cadence)


if __name__ == "__main__": main()
