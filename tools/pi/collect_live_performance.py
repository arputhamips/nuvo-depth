#!/usr/bin/env python3
"""Sample Pi gateway timings through the local relay without changing controls."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import struct
import time
from pathlib import Path
from urllib.request import Request, urlopen


STAGES = (
    "rectification_ms",
    "stereo_preprocess_ms",
    "stereo_match_ms",
    "stereo_postprocess_ms",
    "stereo_ms",
    "processing_ms",
)


def get_bytes(origin: str, path: str) -> tuple[bytes, float]:
    started = time.monotonic()
    with urlopen(Request(origin + path, headers={"Cache-Control": "no-store"}), timeout=10) as response:
        body = response.read()
    return body, (time.monotonic() - started) * 1000


def get_json(origin: str, path: str) -> dict:
    return json.loads(get_bytes(origin, path)[0])


def encoded_metadata(packet: bytes) -> dict:
    if len(packet) < 32 or packet[:4] != b"NVE1":
        raise ValueError("invalid encoded packet")
    metadata_length = struct.unpack_from(">I", packet, 8)[0]
    payload_length = struct.unpack_from(">Q", packet, 12)[0]
    if metadata_length > 65536 or 32 + metadata_length + payload_length != len(packet):
        raise ValueError("encoded packet bounds rejected")
    metadata = json.loads(packet[32 : 32 + metadata_length])
    return {
        "capture_sequence": metadata["capture_sequence"],
        "codec": metadata["codec"],
        "source": metadata["source"],
        "width": metadata["width"],
        "height": metadata["height"],
        "encode_ms": metadata["encode_ms"],
        "payload_bytes": payload_length,
        "packet_bytes": len(packet),
    }


def summary(values: list[float]) -> dict | None:
    if not values:
        return None
    ordered = sorted(values)
    return {
        "count": len(ordered),
        "min": round(ordered[0], 3),
        "median": round(statistics.median(ordered), 3),
        "p95": round(ordered[min(len(ordered) - 1, math.ceil(0.95 * len(ordered)) - 1)], 3),
        "max": round(ordered[-1], 3),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--origin", default="http://127.0.0.1:8767")
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--timeout-seconds", type=float, default=45)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.frames < 2 or args.timeout_seconds <= 0:
        parser.error("frames must be at least 2 and timeout must be positive")

    stereo_before = get_json(args.origin, "/api/v1/stereo")
    encoding_before = get_json(args.origin, "/api/v1/encoding")
    health_samples: list[dict] = []
    encoded_samples: list[dict] = []
    health_rtt: list[float] = []
    encoded_rtt: list[float] = []
    seen_health: set[int] = set()
    seen_encoded: set[int] = set()
    started = time.monotonic()
    deadline = started + args.timeout_seconds
    while len(health_samples) < args.frames and time.monotonic() < deadline:
        body, rtt = get_bytes(args.origin, "/api/v1/health")
        health = json.loads(body)
        if health.get("status") == "running" and health.get("capture_sequence") not in seen_health:
            seen_health.add(health["capture_sequence"])
            health["sample_monotonic_seconds"] = time.monotonic()
            health_samples.append(health)
            health_rtt.append(rtt)
        if encoding_before.get("mode") != "raw":
            try:
                packet, rtt = get_bytes(args.origin, "/api/v1/encoded/latest")
                metadata = encoded_metadata(packet)
                if metadata["capture_sequence"] not in seen_encoded:
                    seen_encoded.add(metadata["capture_sequence"])
                    encoded_samples.append(metadata)
                    encoded_rtt.append(rtt)
            except (OSError, ValueError):
                pass
        time.sleep(0.04)

    stereo_after = get_json(args.origin, "/api/v1/stereo")
    encoding_after = get_json(args.origin, "/api/v1/encoding")
    elapsed = time.monotonic() - started
    stages = {stage: summary([float(frame[stage]) for frame in health_samples if stage in frame]) for stage in STAGES}
    stages["depth_and_other_ms"] = summary([
        max(0.0, frame["processing_ms"] - frame["rectification_ms"]
            - frame["stereo_preprocess_ms"] - frame["stereo_match_ms"]
            - frame["stereo_postprocess_ms"])
        for frame in health_samples
    ])
    stages["encode_ms"] = summary([float(frame["encode_ms"]) for frame in encoded_samples])
    first, last = (health_samples[0], health_samples[-1]) if health_samples else ({}, {})
    observed_seconds = last.get("sample_monotonic_seconds", 0) - first.get("sample_monotonic_seconds", 0)
    result = {
        "schema_version": 1,
        "origin": args.origin,
        "requested_distinct_depth_frames": args.frames,
        "distinct_depth_frames": len(health_samples),
        "distinct_encoded_frames": len(encoded_samples),
        "elapsed_seconds": round(elapsed, 3),
        "stereo_config_before": stereo_before,
        "stereo_config_after": stereo_after,
        "encoding_config_before": encoding_before,
        "encoding_config_after": encoding_after,
        "configuration_stable": stereo_before == stereo_after and encoding_before == encoding_after,
        "backends_seen": sorted({frame.get("stereo_backend", "") for frame in health_samples}),
        "rectification_backends_seen": sorted({frame.get("rectification_backend", "") for frame in health_samples}),
        "grid_sizes": sorted({f'{frame.get("depth_width")}x{frame.get("depth_height")}' for frame in health_samples}),
        "accepted_pair_rate_hz": round((last.get("capture_sequence", 0) - first.get("capture_sequence", 0)) / observed_seconds, 3) if observed_seconds > 0 else None,
        "depth_output_rate_hz": round((last.get("published_frames", 0) - first.get("published_frames", 0)) / observed_seconds, 3) if observed_seconds > 0 else None,
        "sequence_gaps_added": last.get("capture_sequence_gaps", 0) - first.get("capture_sequence_gaps", 0),
        "stages": stages,
        "valid_fraction": summary([float(frame["valid_fraction"]) for frame in health_samples if "valid_fraction" in frame]),
        "searchable_valid_fraction": summary([float(frame["searchable_valid_fraction"]) for frame in health_samples if "searchable_valid_fraction" in frame]),
        "raw_product_payload_bytes": summary([float(frame["payload_bytes"]) for frame in health_samples if "payload_bytes" in frame]),
        "encoded_payload_bytes": summary([float(frame["payload_bytes"]) for frame in encoded_samples]),
        "health_get_rtt_ms": summary(health_rtt),
        "encoded_get_rtt_ms": summary(encoded_rtt),
        "note": "ISP hardware time, capture wait, snapshot I/O, and one-way network latency are not instrumented by these counters. Depth/other is the nonnegative residual, not a separate timer. HTTP RTT includes the local relay and gateway.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in ("distinct_depth_frames", "distinct_encoded_frames", "configuration_stable", "backends_seen", "depth_output_rate_hz", "stages")}, indent=2))
    if len(health_samples) < args.frames:
        raise SystemExit("timed out before the requested number of distinct depth frames")


if __name__ == "__main__":
    main()
