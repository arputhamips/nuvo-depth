from __future__ import annotations

import argparse
import math

from .client import NuvoClient
from .recording import RecordingWriter


def record_frames(client: NuvoClient, writer: RecordingWriter, *, frames: int = 0,
                  cadence_hz: float = 10.0, products: tuple[str, ...] | None = None) -> int:
    """Record a persistent subscription without polling or additive sleeps.

    A normal peer close ends the recording; a timeout or interrupted message
    propagates so callers can distinguish missing data from successful capture.
    """
    if frames < 0:
        raise ValueError("frame count cannot be negative")
    count = 0
    previous = None
    with client.frames(cadence_hz=cadence_hz, products=products) as stream:
        for frame in stream:
            if frame.identity == previous:
                continue
            writer.write(frame)
            previous = frame.identity
            count += 1
            if frames and count >= frames:
                break
    if frames and count < frames:
        raise ConnectionError(f"Nuvo stream ended after {count} of {frames} requested frames")
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description="Record associated Nuvo frame sets")
    parser.add_argument("url")
    parser.add_argument("output")
    parser.add_argument("--claim-token", required=True)
    parser.add_argument("--ca-file")
    parser.add_argument("--frames", type=int, default=0)
    parser.add_argument("--cadence", type=float, default=10.0)
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="seconds allowed for connection or one complete frame")
    parser.add_argument("--products", help="comma-separated raw product names (default: all)")
    args = parser.parse_args()
    if (args.frames < 0 or not math.isfinite(args.cadence) or not 0 < args.cadence <= 60
            or not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("invalid frame count or cadence")
    products = tuple(args.products.split(",")) if args.products is not None else None
    try:
        client = NuvoClient(args.url, ca_file=args.ca_file, timeout=args.timeout)
        # Validate before claiming or creating the output recording.
        client.frames(cadence_hz=args.cadence, products=products)
        client.claim(args.claim_token)
        with RecordingWriter(args.output) as writer:
            record_frames(client, writer, frames=args.frames,
                          cadence_hz=args.cadence, products=products)
    except KeyboardInterrupt:
        return
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"nuvo-record: {error}\n")


if __name__ == "__main__": main()
