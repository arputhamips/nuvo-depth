#!/usr/bin/env python3
"""Sample Pi gateway timings through the local relay without changing controls."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import time
from pathlib import Path
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "sdk/python"))
from nuvo_depth.encoded import decode_encoded_frame

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
    frame = decode_encoded_frame(packet)
    metadata = frame.metadata
    return {
        "run_uuid": metadata.get("run_uuid", ""),
        "capture_epoch": metadata["capture_epoch"],
        "capture_sequence": metadata["capture_sequence"],
        "codec": metadata["codec"],
        "source": metadata["source"],
        "width": metadata["width"],
        "height": metadata["height"],
        "encode_ms": metadata["encode_ms"],
        "payload_bytes": len(frame.data),
        "packet_bytes": len(packet),
    }


def matcher_sample(health: dict) -> tuple[tuple, dict] | None:
    """Observe completed matching, independently of image-only publications."""
    count = health.get("matched_frames")
    if not isinstance(count, int) or isinstance(count, bool) or count <= 0:
        return None
    timing = health.get("matcher_statistics")
    if not isinstance(timing, dict) or timing.get("capture_sequence", 0) <= 0:
        return None
    identity = (health.get("run_uuid"), health.get("capture_epoch"), timing["capture_sequence"])
    sample = {"run_uuid": health.get("run_uuid"), "matched_frames": count, **timing}
    # Quality/grid fields belong to the top-level last processed frame. They
    # must not be relabelled as matcher measurements after mono-only work.
    processed_sequence = health.get("processing_statistics_capture_sequence", health.get("capture_sequence"))
    if (processed_sequence == timing["capture_sequence"]
            and health.get("stereo_backend") not in (None, "not_requested")):
        sample = {**health, **sample}
    return identity, sample


def counter_rate(samples: list[dict], field: str) -> float | None:
    if len(samples) < 2:
        return None
    first, last = samples[0], samples[-1]
    if first.get("run_uuid") != last.get("run_uuid"):
        return None
    before, after = first.get(field), last.get(field)
    seconds = last["sample_monotonic_seconds"] - first["sample_monotonic_seconds"]
    if (not isinstance(before, int) or isinstance(before, bool)
            or not isinstance(after, int) or isinstance(after, bool)
            or after < before or seconds <= 0):
        return None
    return round((after - before) / seconds, 3)


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
    polls: list[dict] = []
    encoded_samples: list[dict] = []
    health_rtt: list[float] = []
    encoded_rtt: list[float] = []
    seen_health: set[tuple] = set()
    seen_encoded: set[tuple] = set()
    started = time.monotonic()
    deadline = started + args.timeout_seconds
    while len(health_samples) < args.frames and time.monotonic() < deadline:
        body, rtt = get_bytes(args.origin, "/api/v1/health")
        health = json.loads(body)
        health["sample_monotonic_seconds"] = time.monotonic()
        polls.append(health)
        health_rtt.append(rtt)
        observed = matcher_sample(health) if health.get("status") == "running" else None
        if observed and observed[0] not in seen_health:
            identity, sample = observed
            seen_health.add(identity)
            sample["sample_monotonic_seconds"] = health["sample_monotonic_seconds"]
            health_samples.append(sample)
        if encoding_before.get("mode") != "raw":
            try:
                packet, rtt = get_bytes(args.origin, "/api/v1/encoded/latest")
                metadata = encoded_metadata(packet)
                identity = (metadata["run_uuid"], metadata["capture_epoch"], metadata["capture_sequence"])
                if identity not in seen_encoded:
                    seen_encoded.add(identity)
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
        for frame in health_samples if all(isinstance(frame.get(stage), (int, float)) and math.isfinite(frame[stage])
                                           for stage in ("processing_ms", "rectification_ms", "stereo_preprocess_ms", "stereo_match_ms", "stereo_postprocess_ms"))
    ])
    stages["encode_ms"] = summary([float(frame["encode_ms"]) for frame in encoded_samples])
    first, last = (polls[0], polls[-1]) if polls else ({}, {})
    identity_stable = len({(frame.get("run_uuid"), frame.get("capture_epoch")) for frame in polls}) <= 1
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
        "daemon_identity_stable": identity_stable,
        "matcher_counter_available": all("matched_frames" in frame for frame in polls),
        "sampling_method": "unique matcher_statistics.capture_sequence; matched_frames counter rate",
        "backends_seen": sorted({frame["stereo_backend"] for frame in health_samples if frame.get("stereo_backend")}),
        "rectification_backends_seen": sorted({frame["rectification_backend"] for frame in health_samples if frame.get("rectification_backend")}),
        "grid_sizes": sorted({f'{frame["depth_width"]}x{frame["depth_height"]}' for frame in health_samples if frame.get("depth_width") and frame.get("depth_height")}),
        "accepted_pair_rate_hz": counter_rate(polls, "captured_pairs") if identity_stable else None,
        "depth_output_rate_hz": counter_rate(polls, "matched_frames") if identity_stable else None,
        "sequence_gaps_added": last.get("capture_sequence_gaps", 0) - first.get("capture_sequence_gaps", 0),
        "stages": stages,
        "valid_fraction": summary([float(frame["valid_fraction"]) for frame in health_samples if "valid_fraction" in frame]),
        "searchable_valid_fraction": summary([float(frame["searchable_valid_fraction"]) for frame in health_samples if "searchable_valid_fraction" in frame]),
        "raw_product_payload_bytes": summary([float(frame["payload_bytes"]) for frame in health_samples if "payload_bytes" in frame]),
        "encoded_payload_bytes": summary([float(frame["payload_bytes"]) for frame in encoded_samples]),
        "health_get_rtt_ms": summary(health_rtt),
        "encoded_get_rtt_ms": summary(encoded_rtt),
        "note": "These are sampled matcher observations, not received depth packets. Older producers without matched_frames/ matcher_statistics cannot provide this measurement. Top-level mono-only processing is excluded. ISP hardware time, capture wait and one-way network latency are not instrumented. Depth/other is a residual, not a separate timer. HTTP RTT includes relay and gateway.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in ("distinct_depth_frames", "distinct_encoded_frames", "configuration_stable", "backends_seen", "depth_output_rate_hz", "stages")}, indent=2))
    if not identity_stable:
        raise SystemExit("daemon restarted during measurement; counter rates unavailable")
    if len(health_samples) < args.frames:
        raise SystemExit("timed out before the requested number of distinct depth frames")


if __name__ == "__main__":
    main()
