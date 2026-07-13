#!/usr/bin/env python3
"""Verify that one GPIO pulse produces exactly one frame on both IMX296s."""

from __future__ import annotations

import argparse
import json
import threading
import time
from pathlib import Path
from typing import Any

from libcamera import Transform
from picamera2 import Picamera2

from capture_rectified_pair import HEIGHT, WIDTH, TriggerLine
from stereo_auto_exposure import ABSOLUTE_MAX_SHUTTER_US


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--left-camera-index", type=int, default=1)
    parser.add_argument("--right-camera-index", type=int, default=0)
    parser.add_argument("--gpiochip", default="/dev/gpiochip0")
    parser.add_argument("--line", type=int, default=17)
    parser.add_argument("--low-pulse-ms", type=float, default=3.0)
    parser.add_argument(
        "--pulse-active-level", choices=("low", "high"), default="low"
    )
    parser.add_argument("--quiet-before-ms", type=float, default=400.0)
    parser.add_argument("--quiet-after-ms", type=float, default=400.0)
    parser.add_argument("--completion-timeout-ms", type=float, default=500.0)
    parser.add_argument("--priming-pulses", type=int, default=5)
    parser.add_argument("--pulse-count", type=int, default=1)
    parser.add_argument("--pulse-period-ms", type=float, default=100.0)
    parser.add_argument(
        "--enable-driver-trigger",
        action="store_true",
        help="set the upstream imx296 module trigger_mode parameter to 1",
    )
    args = parser.parse_args()
    if args.left_camera_index == args.right_camera_index:
        parser.error("left and right camera indices must differ")
    if (
        args.low_pulse_ms <= 0.0
        or args.low_pulse_ms * 1000.0 > ABSOLUTE_MAX_SHUTTER_US
        or args.priming_pulses < 1
        or args.pulse_count < 1
    ):
        parser.error("low pulse must be positive and no longer than 8 ms")
    if args.pulse_period_ms <= args.low_pulse_ms:
        parser.error("pulse period must be longer than pulse width")

    trigger = TriggerLine(args.gpiochip, args.line)
    parameter = Path("/sys/module/imx296/parameters/trigger_mode")
    cameras: list[Picamera2] = []
    started_cameras: list[Picamera2] = []
    condition = threading.Condition()
    completions: list[list[dict[str, Any]]] = [[], []]

    def completed(camera_index: int, request: Any) -> None:
        metadata = request.get_metadata()
        with condition:
            completions[camera_index].append(
                {
                    "callback_monotonic_ns": time.monotonic_ns(),
                    "sensor_timestamp_ns": int(metadata.get("SensorTimestamp", 0)),
                    "exposure_time_us": int(metadata.get("ExposureTime", 0)),
                    "frame_duration_us": int(metadata.get("FrameDuration", 0)),
                }
            )
            condition.notify_all()

    def pulse(count: int, idle_high: bool) -> list[float]:
        durations_ms = []
        for pulse_index in range(count):
            pulse_started_ns = time.monotonic_ns()
            trigger.set(not idle_high)
            time.sleep(args.low_pulse_ms / 1000.0)
            trigger.set(idle_high)
            durations_ms.append(
                (time.monotonic_ns() - pulse_started_ns) / 1_000_000.0
            )
            if pulse_index + 1 < count:
                time.sleep(
                    (args.pulse_period_ms - args.low_pulse_ms) / 1000.0
                )
        return durations_ms

    def wait_for_counts(targets: list[int]) -> None:
        deadline = time.monotonic() + args.completion_timeout_ms / 1000.0
        with condition:
            while any(
                len(completions[index]) < targets[index] for index in range(2)
            ):
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    break
                condition.wait(remaining)

    try:
        idle_high = args.pulse_active_level == "low"
        trigger.set(idle_high)
        if not parameter.exists():
            raise RuntimeError("upstream IMX296 trigger_mode parameter is missing")
        if parameter.read_text().strip() != "1":
            if not args.enable_driver_trigger:
                raise RuntimeError(
                    "IMX296 external trigger is not enabled; run as root with "
                    "--enable-driver-trigger while both streams are stopped"
                )
            parameter.write_text("1\n")
        if parameter.read_text().strip() != "1":
            raise RuntimeError("failed to enable IMX296 external trigger mode")

        cameras = [
            Picamera2(args.left_camera_index),
            Picamera2(args.right_camera_index),
        ]
        transform = Transform(hflip=1, vflip=1)
        for camera_index, camera in enumerate(cameras):
            configuration = camera.create_video_configuration(
                main={"size": (WIDTH, HEIGHT), "format": "RGB888"},
                controls={
                    "FrameDurationLimits": (100_000, 100_000),
                    "ExposureTime": round(args.low_pulse_ms * 1000.0),
                    "AnalogueGain": 1.0,
                },
                transform=transform,
                buffer_count=4,
            )
            camera.configure(configuration)
            camera.post_callback = lambda request, index=camera_index: completed(
                index, request
            )

        for camera in cameras:
            camera.start()
            started_cameras.append(camera)

        time.sleep(args.quiet_before_ms / 1000.0)
        with condition:
            before_priming = [len(camera_frames) for camera_frames in completions]

        priming_durations_ms = pulse(args.priming_pulses, idle_high)
        wait_for_counts([1, 1])
        with condition:
            after_priming = [len(camera_frames) for camera_frames in completions]

        test_durations_ms = pulse(args.pulse_count, idle_high)
        wait_for_counts(
            [after_priming[index] + args.pulse_count for index in range(2)]
        )

        time.sleep(args.quiet_after_ms / 1000.0)
        with condition:
            after = [len(camera_frames) for camera_frames in completions]
            new_frames = [
                completions[index][after_priming[index] :] for index in range(2)
            ]
    finally:
        for camera in reversed(started_cameras):
            camera.stop()
        for camera in cameras:
            camera.close()
        trigger.close()

    expected_each = [len(frames) for frames in new_frames] == [
        args.pulse_count,
        args.pulse_count,
    ]
    timestamp_delta_ns = (
        abs(
            new_frames[0][0]["sensor_timestamp_ns"]
            - new_frames[1][0]["sensor_timestamp_ns"]
        )
        if expected_each and args.pulse_count == 1
        else None
    )
    report = {
        "driver_trigger_mode": parameter.read_text().strip(),
        "gpiochip": args.gpiochip,
        "line": args.line,
        "pulse_active_level": args.pulse_active_level,
        "low_pulse_ms_requested": args.low_pulse_ms,
        "priming_pulses": args.priming_pulses,
        "pulse_count": args.pulse_count,
        "pulse_period_ms": args.pulse_period_ms,
        "low_pulse_ms_measured": {
            "priming": priming_durations_ms,
            "test": test_durations_ms,
        },
        "completions_before_priming": before_priming,
        "completions_after_priming": after_priming,
        "completions_after_quiet_window": after,
        "new_frames_after_test_pulses": [len(frames) for frames in new_frames],
        "new_frame_metadata": new_frames,
        "sensor_timestamp_delta_ns": timestamp_delta_ns,
        "pass": (
            expected_each
            and before_priming == [0, 0]
            and all(count >= 1 for count in after_priming)
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    if not report["pass"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
