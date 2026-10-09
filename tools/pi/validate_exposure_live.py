#!/usr/bin/env python3
"""Run a bounded live shared-auto exposure check and restore device settings."""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import time
from pathlib import Path
from typing import Any


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def apply(configuration: dict[str, Any]) -> None:
    subprocess.run(
        [
            "/usr/bin/nuvoctl",
            "exposure",
            "set",
            "--json",
            json.dumps(configuration, separators=(",", ":")),
        ],
        check=True,
        capture_output=True,
        text=True,
    )


def hardware_value(command: str) -> str | None:
    try:
        return subprocess.run(
            command.split(), check=True, capture_output=True, text=True
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def wait_for(
    status_path: Path, predicate: Any, deadline: float
) -> dict[str, Any]:
    while time.monotonic() < deadline:
        try:
            status = read_json(status_path)
            if predicate(status):
                return status
        except (OSError, json.JSONDecodeError):
            pass
        time.sleep(0.05)
    raise TimeoutError("timed out waiting for live exposure state")


def numeric(values: list[dict[str, Any]], key: str) -> list[float]:
    return [float(value[key]) for value in values if value.get(key) is not None]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--timeout-seconds", type=float, default=180.0)
    parser.add_argument(
        "--exposure", type=Path, default=Path("/etc/nuvo-depth/exposure.json")
    )
    parser.add_argument(
        "--status", type=Path, default=Path("/run/nuvo-depth/status.json")
    )
    args = parser.parse_args()
    if args.frames < 1:
        raise ValueError("--frames must be positive")

    original = read_json(args.exposure)
    if original.get("mode") != "manual":
        raise ValueError(
            "live validation must start in Manual so the Auto transition "
            "and exact restoration are observable"
        )
    initial = read_json(args.status)
    automatic = dict(original)
    automatic["mode"] = "auto"
    deadline = time.monotonic() + args.timeout_seconds
    samples: list[dict[str, Any]] = []
    restored: dict[str, Any] | None = None
    temperature_start = hardware_value("vcgencmd measure_temp")
    throttled_start = hardware_value("vcgencmd get_throttled")
    try:
        apply(automatic)
        active = wait_for(
            args.status,
            lambda value: value.get("exposure_mode") == "auto"
            and int(value.get("exposure_configuration_reloads", 0))
            > int(initial.get("exposure_configuration_reloads", 0)),
            deadline,
        )
        start_published = int(active["published_frames"])
        previous = start_published - 1
        while len(samples) < args.frames and time.monotonic() < deadline:
            status = read_json(args.status)
            published = int(status["published_frames"])
            if published != previous:
                samples.append(status)
                previous = published
            time.sleep(0.03)
        if len(samples) < args.frames:
            raise TimeoutError(
                f"captured {len(samples)} of {args.frames} requested status frames"
            )
    finally:
        apply(original)
        restored = wait_for(
            args.status,
            lambda value: value.get("exposure_mode") == original["mode"],
            time.monotonic() + 30.0,
        )

    exposures = numeric(samples, "requested_exposure_us")
    gains = numeric(samples, "requested_analogue_gain")
    medians = numeric(samples, "metered_median")
    differences = numeric(samples, "metered_median_difference")
    clipped = numeric(samples, "metered_clipped_fraction")
    left_reported = numeric(samples, "left_reported_exposure_us")
    right_reported = numeric(samples, "right_reported_exposure_us")
    left_gain = numeric(samples, "left_reported_analogue_gain")
    right_gain = numeric(samples, "right_reported_analogue_gain")
    first = samples[0]
    last = samples[-1]
    report = {
        "schema_version": 1,
        "frames_sampled": len(samples),
        "published_frame_span": int(last["published_frames"])
        - int(first["published_frames"])
        + 1,
        "capture_sequence_span": int(last["capture_sequence"])
        - int(first["capture_sequence"])
        + 1,
        "mode_transition": {
            "before_exposure_us": initial.get("requested_exposure_us"),
            "before_gain": initial.get("requested_analogue_gain"),
            "first_auto_exposure_us": first.get("requested_exposure_us"),
            "first_auto_gain": first.get("requested_analogue_gain"),
            "restored_mode": restored.get("exposure_mode") if restored else None,
            "restored_exposure_us": (
                restored.get("requested_exposure_us") if restored else None
            ),
            "restored_gain": (
                restored.get("requested_analogue_gain") if restored else None
            ),
        },
        "controller": {
            "updates": int(last["exposure_updates"])
            - int(first["exposure_updates"]),
            "configuration_errors": last.get("exposure_configuration_errors"),
            "states": sorted(
                {str(value.get("exposure_controller_state")) for value in samples}
            ),
            "exposure_us_min": min(exposures),
            "exposure_us_max": max(exposures),
            "gain_min": min(gains),
            "gain_max": max(gains),
            "at_upper_limit": last.get("exposure_at_upper_limit"),
            "safe_max_exposure_us": last.get("exposure_safe_max_us"),
            "frame_period_us": last.get("exposure_frame_period_us"),
            "trigger_guard_us": last.get("exposure_trigger_guard_us"),
        },
        "metering": {
            "median_min": min(medians),
            "median_max": max(medians),
            "median_mean": statistics.fmean(medians),
            "stereo_median_difference_max": max(differences),
            "clipped_fraction_max": max(clipped),
        },
        "stereo_controls": {
            "reported_exposure_difference_max_us": max(
                abs(left - right)
                for left, right in zip(left_reported, right_reported, strict=True)
            ),
            "reported_analogue_gain_difference_max": max(
                abs(left - right)
                for left, right in zip(left_gain, right_gain, strict=True)
            ),
        },
        "trigger": {
            "attempts_delta": int(last["trigger_attempts"])
            - int(first["trigger_attempts"]),
            "deadline_overruns": last.get("trigger_deadline_overruns"),
            "errors": last.get("trigger_errors"),
            "process_pulse_min_us": last.get("trigger_min_pulse_us"),
            "process_pulse_max_us": last.get("trigger_max_pulse_us"),
            "pulse_guard_overruns_lifetime": last.get(
                "trigger_pulse_guard_overruns"
            ),
            "pulse_guard_overruns_during_samples": int(
                last.get("trigger_pulse_guard_overruns", 0)
            )
            - int(first.get("trigger_pulse_guard_overruns", 0)),
            "maximum_pulse_overshoot_us": last.get(
                "trigger_max_pulse_overshoot_us"
            ),
            "maximum_pair_delta_ns": last.get("maximum_pair_delta_ns"),
        },
        "pipeline": {
            "rectification_backend": last.get("rectification_backend"),
            "stereo_backend": last.get("stereo_backend"),
            "processing_ms_last": last.get("processing_ms"),
            "valid_fraction_last": last.get("valid_fraction"),
        },
        "hardware": {
            "temperature_start": temperature_start,
            "temperature_end": hardware_value("vcgencmd measure_temp"),
            "throttled_start": throttled_start,
            "throttled_end": hardware_value("vcgencmd get_throttled"),
        },
    }
    print(json.dumps(report, sort_keys=True, indent=2))


if __name__ == "__main__":
    main()
