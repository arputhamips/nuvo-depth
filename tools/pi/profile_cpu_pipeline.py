#!/usr/bin/env python3
"""Profile capture-to-depth stages for the current CPU SGBM pipeline."""

from __future__ import annotations

import argparse
import json
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

import cv2
import numpy as np
from libcamera import Transform
from picamera2 import Picamera2

from capture_rectified_pair import TriggerLine, calibration, pulse_loop


WIDTH = 728
HEIGHT = 544
WORKING_WIDTH = 364
WORKING_HEIGHT = 272
WORKING_DISPARITIES = 96
FULL_DISPARITY_LIMIT = 192.0
FOCAL_LENGTH_PX = 493.542046188
BASELINE_M = 0.161458780


def milliseconds(start_ns: int) -> float:
    return (time.perf_counter_ns() - start_ns) / 1_000_000.0


def percentile(values: list[float], quantile: float) -> float:
    return float(np.percentile(np.asarray(values), quantile)) if values else 0.0


def summaries(samples: dict[str, list[float]]) -> dict[str, dict[str, float]]:
    return {
        name: {
            "first_ms": values[0],
            "p50_ms": percentile(values, 50),
            "p95_ms": percentile(values, 95),
            "max_ms": max(values),
        }
        for name, values in samples.items()
        if values
    }


def system_cpu_ticks() -> tuple[int, int]:
    fields = Path("/proc/stat").read_text().splitlines()[0].split()[1:]
    ticks = [int(value) for value in fields]
    idle = ticks[3] + (ticks[4] if len(ticks) > 4 else 0)
    return sum(ticks), idle


def make_maps(path: Path) -> tuple[np.ndarray, np.ndarray]:
    matrix, distortion, rectification, projection = calibration(path)
    return cv2.initUndistortRectifyMap(
        matrix,
        distortion,
        rectification,
        projection[:, :3],
        (WIDTH, HEIGHT),
        cv2.CV_16SC2,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--left-calibration", type=Path, required=True)
    parser.add_argument("--right-calibration", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--left-camera-index", type=int, default=1)
    parser.add_argument("--right-camera-index", type=int, default=0)
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--warmup-frames", type=int, default=3)
    parser.add_argument("--fps", type=float, default=10.0)
    parser.add_argument("--jpeg-hz", type=float, default=2.0)
    parser.add_argument("--jpeg-quality", type=int, default=75)
    parser.add_argument(
        "--capture-flush",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="require each request's exposure to start after the capture call",
    )
    parser.add_argument("--trigger-start-delay", type=float, default=4.0)
    parser.add_argument("--gpiochip", default="/dev/gpiochip0")
    parser.add_argument("--line", type=int, default=17)
    args = parser.parse_args()
    if args.frames < 1 or args.warmup_frames < 0 or args.fps <= 0:
        parser.error("frame counts and fps must be positive")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    cv2.setNumThreads(1)

    cold: dict[str, float] = {}
    cold_started = time.perf_counter_ns()
    stage_started = time.perf_counter_ns()
    left_maps = make_maps(args.left_calibration)
    right_maps = make_maps(args.right_calibration)
    cold["rectification_map_build_ms"] = milliseconds(stage_started)

    stage_started = time.perf_counter_ns()
    cameras = [Picamera2(args.left_camera_index), Picamera2(args.right_camera_index)]
    controls = {
        "FrameDurationLimits": (
            round(1_000_000 / args.fps),
            round(1_000_000 / args.fps),
        ),
        "SyncMode": 2,
    }
    transform = Transform(hflip=1, vflip=1)
    for camera in cameras:
        configuration = camera.create_video_configuration(
            main={"size": (WIDTH, HEIGHT), "format": "RGB888"},
            controls=controls,
            transform=transform,
            buffer_count=4,
        )
        camera.configure(configuration)
    cold["camera_open_and_configure_ms"] = milliseconds(stage_started)

    stage_started = time.perf_counter_ns()
    block_size = 5
    matcher = cv2.StereoSGBM_create(
        0,
        WORKING_DISPARITIES,
        block_size,
        8 * block_size * block_size,
        32 * block_size * block_size,
        1,
        31,
        5,
        50,
        2,
        cv2.STEREO_SGBM_MODE_SGBM_3WAY,
    )
    cold["matcher_create_ms"] = milliseconds(stage_started)

    trigger = TriggerLine(args.gpiochip, args.line)
    trigger_stop = threading.Event()
    trigger_thread = threading.Thread(
        target=pulse_loop, args=(trigger, trigger_stop), daemon=True
    )
    trigger_started = False
    samples: dict[str, list[float]] = {}
    pair_deltas_ns: list[int] = []
    frame_ages_ms: list[list[float]] = [[], []]
    sensor_timestamps_ns: list[list[int]] = [[], []]
    sync_ready_pairs = 0
    jpeg_sizes: list[int] = []
    last: dict[str, np.ndarray] = {}

    def record(name: str, value: float, enabled: bool) -> None:
        if enabled:
            samples.setdefault(name, []).append(value)

    def capture_one(
        camera: Picamera2,
    ) -> tuple[np.ndarray, dict[str, Any], int, float, float]:
        request_started = time.perf_counter_ns()
        request = camera.capture_request(flush=args.capture_flush)
        request_ms = (time.perf_counter_ns() - request_started) / 1_000_000.0
        try:
            copy_started = time.perf_counter_ns()
            image = request.make_array("main").copy()
            copy_ms = (time.perf_counter_ns() - copy_started) / 1_000_000.0
            metadata = request.get_metadata()
            return image, metadata, time.monotonic_ns(), request_ms, copy_ms
        finally:
            request.release()

    jpeg_period = max(1, round(args.fps / max(args.jpeg_hz, 1e-6)))
    try:
        stage_started = time.perf_counter_ns()
        for camera in cameras:
            camera.start()
        cold["camera_start_calls_ms"] = milliseconds(stage_started)

        stage_started = time.perf_counter_ns()
        time.sleep(args.trigger_start_delay)
        trigger_thread.start()
        trigger_started = True
        time.sleep(0.5)
        cold["trigger_stabilization_ms"] = milliseconds(stage_started)
        cold["initialization_to_capture_ready_ms"] = milliseconds(cold_started)

        with ThreadPoolExecutor(max_workers=2) as executor:
            measured_cpu_started = 0.0
            measured_wall_started = 0
            total_frames = args.warmup_frames + args.frames
            for frame_index in range(total_frames):
                measured = frame_index >= args.warmup_frames
                if frame_index == args.warmup_frames:
                    measured_cpu_started = time.process_time()
                    measured_wall_started = time.perf_counter_ns()
                    system_ticks_started = system_cpu_ticks()
                cycle_started = time.perf_counter_ns()

                stage_started = time.perf_counter_ns()
                futures = [executor.submit(capture_one, camera) for camera in cameras]
                captured = [future.result() for future in futures]
                record("capture_wait_and_copy_ms", milliseconds(stage_started), measured)
                images = [captured[0][0], captured[1][0]]
                metadata = [captured[0][1], captured[1][1]]
                pair_ready_ns = time.monotonic_ns()
                for camera_index, side in enumerate(("left", "right")):
                    record(
                        f"capture_{side}_request_wait_ms",
                        captured[camera_index][3],
                        measured,
                    )
                    record(
                        f"capture_{side}_array_copy_ms",
                        captured[camera_index][4],
                        measured,
                    )
                    timestamp = int(
                        metadata[camera_index].get("SensorTimestamp", 0)
                    )
                    record(
                        f"pair_ready_{side}_frame_age_ms",
                        (pair_ready_ns - timestamp) / 1_000_000.0,
                        measured,
                    )
                pair_delta = abs(
                    int(metadata[0].get("SensorTimestamp", 0))
                    - int(metadata[1].get("SensorTimestamp", 0))
                )
                if measured:
                    pair_deltas_ns.append(pair_delta)
                    for camera_index in range(2):
                        timestamp = int(
                            metadata[camera_index].get("SensorTimestamp", 0)
                        )
                        sensor_timestamps_ns[camera_index].append(timestamp)
                        frame_ages_ms[camera_index].append(
                            (captured[camera_index][2] - timestamp) / 1_000_000.0
                        )
                    sync_ready_pairs += int(
                        bool(metadata[0].get("SyncReady", False))
                        and bool(metadata[1].get("SyncReady", False))
                    )

                stage_started = time.perf_counter_ns()
                left = cv2.remap(
                    images[0], left_maps[0], left_maps[1], cv2.INTER_LINEAR
                )
                right = cv2.remap(
                    images[1], right_maps[0], right_maps[1], cv2.INTER_LINEAR
                )
                record("rectification_ms", milliseconds(stage_started), measured)

                stage_started = time.perf_counter_ns()
                left_gray = cv2.cvtColor(left, cv2.COLOR_BGR2GRAY)
                right_gray = cv2.cvtColor(right, cv2.COLOR_BGR2GRAY)
                record("grayscale_ms", milliseconds(stage_started), measured)

                stage_started = time.perf_counter_ns()
                left_working = cv2.resize(
                    left_gray,
                    (WORKING_WIDTH, WORKING_HEIGHT),
                    interpolation=cv2.INTER_AREA,
                )
                right_working = cv2.resize(
                    right_gray,
                    (WORKING_WIDTH, WORKING_HEIGHT),
                    interpolation=cv2.INTER_AREA,
                )
                record("downscale_ms", milliseconds(stage_started), measured)

                stage_started = time.perf_counter_ns()
                disparity_fixed = matcher.compute(left_working, right_working)
                record("sgbm_match_ms", milliseconds(stage_started), measured)

                stage_started = time.perf_counter_ns()
                valid_working = disparity_fixed > 0
                disparity_working = disparity_fixed.astype(np.float32) / 16.0
                disparity = cv2.resize(
                    disparity_working,
                    (WIDTH, HEIGHT),
                    interpolation=cv2.INTER_LINEAR,
                )
                disparity *= WIDTH / WORKING_WIDTH
                valid = cv2.resize(
                    valid_working.astype(np.uint8),
                    (WIDTH, HEIGHT),
                    interpolation=cv2.INTER_NEAREST,
                ).astype(bool)
                valid &= np.isfinite(disparity)
                valid &= disparity > 0.0
                valid &= disparity < FULL_DISPARITY_LIMIT
                disparity[~valid] = np.nan
                record("disparity_restore_and_mask_ms", milliseconds(stage_started), measured)

                stage_started = time.perf_counter_ns()
                depth = np.full(disparity.shape, np.nan, dtype=np.float32)
                np.divide(
                    FOCAL_LENGTH_PX * BASELINE_M,
                    disparity,
                    out=depth,
                    where=valid,
                )
                record("metric_depth_ms", milliseconds(stage_started), measured)

                if measured and (frame_index - args.warmup_frames) % jpeg_period == 0:
                    stage_started = time.perf_counter_ns()
                    encoded, jpeg = cv2.imencode(
                        ".jpg",
                        left,
                        [cv2.IMWRITE_JPEG_QUALITY, args.jpeg_quality],
                    )
                    if not encoded:
                        raise RuntimeError("JPEG encoding failed")
                    jpeg_sizes.append(int(jpeg.size))
                    record("jpeg_encode_ms", milliseconds(stage_started), True)

                stage_started = time.perf_counter_ns()
                published_disparity = disparity.copy()
                published_depth = depth.copy()
                record("output_buffer_copy_ms", milliseconds(stage_started), measured)
                output_ready_ns = time.monotonic_ns()
                for camera_index, side in enumerate(("left", "right")):
                    timestamp = int(
                        metadata[camera_index].get("SensorTimestamp", 0)
                    )
                    record(
                        f"depth_ready_{side}_frame_age_ms",
                        (output_ready_ns - timestamp) / 1_000_000.0,
                        measured,
                    )
                record("cycle_ms", milliseconds(cycle_started), measured)
                if measured:
                    last = {
                        "left_raw": images[0],
                        "right_raw": images[1],
                        "left": left,
                        "right": right,
                        "disparity": published_disparity,
                        "depth": published_depth,
                    }

            measured_wall_s = (
                time.perf_counter_ns() - measured_wall_started
            ) / 1_000_000_000.0
            measured_cpu_s = time.process_time() - measured_cpu_started
            system_ticks_finished = system_cpu_ticks()
    finally:
        trigger_stop.set()
        if trigger_started:
            trigger_thread.join(timeout=1.0)
        trigger.close()
        for camera in cameras:
            camera.stop()
            camera.close()

    logical_cpus = 4
    process_cpu_percent = 100.0 * measured_cpu_s / measured_wall_s
    system_total_delta = system_ticks_finished[0] - system_ticks_started[0]
    system_idle_delta = system_ticks_finished[1] - system_ticks_started[1]
    system_soc_cpu_percent = (
        100.0 * (system_total_delta - system_idle_delta) / system_total_delta
    )
    report = {
        "configuration": {
            "input": [WIDTH, HEIGHT],
            "working": [WORKING_WIDTH, WORKING_HEIGHT],
            "working_disparities": WORKING_DISPARITIES,
            "full_disparity_limit": FULL_DISPARITY_LIMIT,
            "opencv_threads": cv2.getNumThreads(),
            "frames": args.frames,
            "warmup_frames": args.warmup_frames,
            "capture_fps": args.fps,
            "capture_flush": args.capture_flush,
            "jpeg_hz": args.jpeg_hz,
        },
        "cold_start": cold,
        "steady_state": {
            "wall_seconds": measured_wall_s,
            "achieved_hz": args.frames / measured_wall_s,
            "process_cpu_percent_one_core_100": process_cpu_percent,
            "soc_cpu_percent_four_core_100": process_cpu_percent / logical_cpus,
            "whole_soc_cpu_percent": system_soc_cpu_percent,
            "stages": summaries(samples),
            "sensor_delta_ns": {
                "first": pair_deltas_ns[0],
                "p50": percentile([float(value) for value in pair_deltas_ns], 50),
                "p95": percentile([float(value) for value in pair_deltas_ns], 95),
            },
            "frame_age_ms": {
                side: {
                    "p50": percentile(frame_ages_ms[index], 50),
                    "p95": percentile(frame_ages_ms[index], 95),
                }
                for index, side in enumerate(("left", "right"))
            },
            "sensor_period_ms": {
                side: {
                    "p50": percentile(
                        [
                            (timestamps[index] - timestamps[index - 1])
                            / 1_000_000.0
                            for index in range(1, len(timestamps))
                        ],
                        50,
                    ),
                    "p95": percentile(
                        [
                            (timestamps[index] - timestamps[index - 1])
                            / 1_000_000.0
                            for index in range(1, len(timestamps))
                        ],
                        95,
                    ),
                }
                for side, timestamps in zip(
                    ("left", "right"), sensor_timestamps_ns
                )
            },
            "sync_ready_pair_fraction": sync_ready_pairs / args.frames,
            "jpeg_samples": len(jpeg_sizes),
            "jpeg_mean_bytes": float(np.mean(jpeg_sizes)) if jpeg_sizes else 0.0,
            "valid_fraction": float(np.isfinite(last["depth"]).mean()),
        },
    }

    artifact_started = time.perf_counter_ns()
    cv2.imwrite(str(args.output_dir / "left_raw.png"), last["left_raw"])
    cv2.imwrite(str(args.output_dir / "right_raw.png"), last["right_raw"])
    cv2.imwrite(str(args.output_dir / "left_rect.png"), last["left"])
    cv2.imwrite(str(args.output_dir / "right_rect.png"), last["right"])
    np.save(args.output_dir / "disparity_float32.npy", last["disparity"])
    np.save(args.output_dir / "depth_m_float32.npy", last["depth"])
    report["diagnostic_artifact_write_ms"] = milliseconds(artifact_started)
    (args.output_dir / "pipeline_profile.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
