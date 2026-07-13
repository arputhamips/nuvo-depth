#!/usr/bin/env python3
"""Profile async leased-buffer capture through CPU SGBM depth output."""

from __future__ import annotations

import argparse
import json
import resource
import time
from pathlib import Path

import cv2
import numpy as np

from async_stereo_capture import AsyncStereoCapture
from profile_cpu_pipeline import (
    BASELINE_M,
    FOCAL_LENGTH_PX,
    FULL_DISPARITY_LIMIT,
    HEIGHT,
    WIDTH,
    WORKING_DISPARITIES,
    WORKING_HEIGHT,
    WORKING_WIDTH,
    make_maps,
    milliseconds,
    percentile,
    summaries,
    system_cpu_ticks,
)
from stereo_auto_exposure import AutoExposureConfig, StereoAutoExposure


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--left-calibration", type=Path, required=True)
    parser.add_argument("--right-calibration", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--left-camera-index", type=int, default=1)
    parser.add_argument("--right-camera-index", type=int, default=0)
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--warmup-frames", type=int, default=6)
    parser.add_argument("--fps", type=float, default=10.0)
    parser.add_argument("--jpeg-hz", type=float, default=2.0)
    parser.add_argument("--jpeg-quality", type=int, default=75)
    parser.add_argument(
        "--metric-depth",
        action="store_true",
        help="derive and save metric depth in addition to disparity",
    )
    parser.add_argument("--trigger-low-ms", type=float, default=8.0)
    parser.add_argument("--analogue-gain", type=float, default=2.0)
    parser.add_argument(
        "--colour-gains",
        type=float,
        nargs=2,
        metavar=("RED", "BLUE"),
        default=(2.4771, 2.3425),
    )
    parser.add_argument(
        "--auto-exposure",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    parser.add_argument("--ae-target-median", type=float, default=75.0)
    parser.add_argument("--ae-median-low", type=float, default=65.0)
    parser.add_argument("--ae-median-high", type=float, default=85.0)
    parser.add_argument("--ae-min-shutter-us", type=int, default=250)
    parser.add_argument("--ae-max-shutter-us", type=int, default=8000)
    parser.add_argument("--ae-max-gain", type=float, default=3.0)
    parser.add_argument("--ae-update-interval", type=int, default=2)
    parser.add_argument("--ae-settle-frames", type=int, default=4)
    parser.add_argument("--ae-max-clipped-fraction", type=float, default=0.005)
    parser.add_argument("--pair-tolerance-us", type=float, default=500.0)
    parser.add_argument("--working-width", type=int, default=WORKING_WIDTH)
    parser.add_argument("--working-height", type=int, default=WORKING_HEIGHT)
    parser.add_argument("--min-disparity", type=int, default=0)
    parser.add_argument(
        "--working-disparities",
        type=int,
        default=WORKING_DISPARITIES,
        help="SGBM disparity search range at working resolution (multiple of 16)",
    )
    parser.add_argument(
        "--full-disparity-limit",
        type=float,
        default=FULL_DISPARITY_LIMIT,
        help="maximum accepted disparity after restoration to input resolution",
    )
    parser.add_argument("--block-size", type=int, default=5)
    parser.add_argument("--uniqueness-ratio", type=int, default=1)
    parser.add_argument("--speckle-window-size", type=int, default=30)
    parser.add_argument("--speckle-range", type=int, default=2)
    parser.add_argument("--disp12-max-diff", type=int, default=1)
    parser.add_argument("--gpiochip", default="/dev/gpiochip0")
    parser.add_argument("--line", type=int, default=17)
    parser.add_argument(
        "--result-ring",
        type=Path,
        help="publish native disparity/cloud/image products to this mmap ring",
    )
    parser.add_argument(
        "--native-output-library",
        type=Path,
        help="path to libdepthd_native_output.so",
    )
    parser.add_argument(
        "--publish-left-mono-to-ring",
        action="store_true",
        help="include rectified mono for an explicit diagnostic consumer",
    )
    parser.add_argument(
        "--semantic",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="run native semantic perception (disabled by default)",
    )
    parser.add_argument(
        "--semantic-ring",
        type=Path,
        default=Path("/run/depthd/semantic.ring"),
    )
    parser.add_argument(
        "--semantic-output-library",
        type=Path,
        help="path to libdepthd_semantic_output.so",
    )
    parser.add_argument(
        "--semantic-yolo-hef",
        type=Path,
        default=Path("/home/cam/nuvo-base/data/models/hailo/front_semantic.hef"),
    )
    parser.add_argument(
        "--semantic-segformer-hef",
        type=Path,
        default=Path("/home/cam/nuvo-base/data/models/hailo/segformer_b0_bn.hef"),
    )
    parser.add_argument("--semantic-yolo-hz", type=float, default=5.0)
    parser.add_argument("--semantic-segformer-hz", type=float, default=5.0)
    parser.add_argument(
        "--semantic-preview",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="include a diagnostic JPEG in semantic.ring",
    )
    args = parser.parse_args()
    if args.frames < 1 or args.warmup_frames < 0 or args.fps <= 0.0:
        parser.error("frame counts and fps must be positive")
    if args.semantic and args.result_ring is None:
        parser.error("--semantic requires --result-ring for source sequence parity")
    if args.semantic_yolo_hz <= 0.0 or args.semantic_segformer_hz <= 0.0:
        parser.error("semantic model rates must be positive")
    if (
        args.working_disparities < 16
        or args.working_disparities % 16 != 0
        or args.working_width < 1
        or args.working_height < 1
        or args.working_width > WIDTH
        or args.working_height > HEIGHT
        or args.working_disparities >= args.working_width
        or args.full_disparity_limit <= 0.0
        or args.block_size < 1
        or args.block_size % 2 == 0
        or args.uniqueness_ratio < 0
        or args.speckle_window_size < 0
        or args.speckle_range < 0
    ):
        parser.error("invalid SGBM filtering parameters")
    try:
        ae_config = AutoExposureConfig(
            target_median=args.ae_target_median,
            median_low=args.ae_median_low,
            median_high=args.ae_median_high,
            max_clipped_fraction=args.ae_max_clipped_fraction,
            min_shutter_us=args.ae_min_shutter_us,
            max_shutter_us=args.ae_max_shutter_us,
            max_gain=args.ae_max_gain,
            update_interval_frames=args.ae_update_interval,
            settle_frames_after_update=args.ae_settle_frames,
            roi_x_start=round(160 * args.working_width / WIDTH),
            roi_y_start=round(96 * args.working_height / HEIGHT),
            roi_y_end=round(420 * args.working_height / HEIGHT),
        )
        ae_config.validate()
        auto_exposure = (
            StereoAutoExposure(
                ae_config,
                initial_shutter_us=round(args.trigger_low_ms * 1000.0),
                initial_gain=args.analogue_gain,
            )
            if args.auto_exposure
            else None
        )
    except ValueError as error:
        parser.error(str(error))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    cv2.setNumThreads(1)

    left_maps = make_maps(args.left_calibration)
    right_maps = make_maps(args.right_calibration)
    block_size = args.block_size
    matcher = cv2.StereoSGBM_create(
        args.min_disparity,
        args.working_disparities,
        block_size,
        8 * block_size * block_size,
        32 * block_size * block_size,
        args.disp12_max_diff,
        31,
        args.uniqueness_ratio,
        args.speckle_window_size,
        args.speckle_range,
        cv2.STEREO_SGBM_MODE_SGBM_3WAY,
    )
    capture = AsyncStereoCapture(
        left_camera_index=args.left_camera_index,
        right_camera_index=args.right_camera_index,
        gpiochip=args.gpiochip,
        trigger_line=args.line,
        frequency_hz=args.fps,
        trigger_low_ms=args.trigger_low_ms,
        analogue_gain=args.analogue_gain,
        colour_gains=tuple(args.colour_gains),
        pair_tolerance_us=args.pair_tolerance_us,
    )
    native_output = None
    semantic_worker = None
    sensor_to_realtime_ns = None
    if args.result_ring is not None:
        from depthd_native_output import (
            NativeOutputPublisher,
            sensor_monotonic_to_realtime_ns,
        )

        native_output = NativeOutputPublisher(
            args.result_ring,
            args.native_output_library,
        )
        sensor_to_realtime_ns = sensor_monotonic_to_realtime_ns
    if args.semantic:
        from depthd_semantic import (
            SemanticOutputPublisher,
            SemanticWorker,
            make_hailo_backends,
        )

        try:
            registrations, hailo_owner = make_hailo_backends(
                args.semantic_yolo_hef,
                args.semantic_segformer_hef,
                yolo_hz=args.semantic_yolo_hz,
                segformer_hz=args.semantic_segformer_hz,
            )
            semantic_output = SemanticOutputPublisher(
                args.semantic_ring,
                args.semantic_output_library,
            )
            semantic_worker = SemanticWorker(
                registrations,
                semantic_output,
                preview_enabled=args.semantic_preview,
            )
        except Exception:
            if "hailo_owner" in locals():
                hailo_owner.close()
            if native_output is not None:
                native_output.close()
            raise

    samples: dict[str, list[float]] = {}
    sensor_deltas_ns: list[int] = []
    callback_deltas_ns: list[int] = []
    jpeg_sizes: list[int] = []
    valid_fractions: list[float] = []
    left_applied_gains: list[float] = []
    right_applied_gains: list[float] = []
    applied_gain_deltas: list[float] = []
    last_jpeg: np.ndarray | None = None
    last: dict[str, np.ndarray] = {}
    last_native_output: dict[str, float | int] | None = None
    jpeg_period = max(1, round(args.fps / max(args.jpeg_hz, 1e-6)))

    def record(name: str, started_ns: int, measured: bool) -> None:
        if measured:
            samples.setdefault(name, []).append(milliseconds(started_ns))

    jpeg_warmup_started_ns = time.perf_counter_ns()
    warmed, _ = cv2.imencode(
        ".jpg",
        np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8),
        [cv2.IMWRITE_JPEG_QUALITY, args.jpeg_quality],
    )
    if not warmed:
        raise RuntimeError("JPEG warm-up failed")
    jpeg_warmup_ms = milliseconds(jpeg_warmup_started_ns)

    capture_started_ns = time.perf_counter_ns()
    try:
        capture.start()
    except Exception:
        if semantic_worker is not None:
            semantic_worker.close()
        if native_output is not None:
            native_output.close()
        raise
    capture_start_ms = milliseconds(capture_started_ns)
    try:
        total_frames = args.warmup_frames + args.frames
        for frame_index in range(total_frames):
            measured = frame_index >= args.warmup_frames
            jpeg_due = (
                measured
                and (frame_index - args.warmup_frames) % jpeg_period == 0
            )
            if frame_index == args.warmup_frames:
                measured_wall_started_ns = time.perf_counter_ns()
                measured_process_cpu_started = time.process_time()
                measured_system_ticks_started = system_cpu_ticks()
            cycle_started_ns = time.perf_counter_ns()
            semantic_schedule_ns = time.monotonic_ns()
            semantic_due = (
                semantic_worker.wants_frame(semantic_schedule_ns)
                if semantic_worker is not None
                else False
            )

            stage_started_ns = time.perf_counter_ns()
            pair = capture.take(timeout_s=2.0)
            if pair is None:
                raise RuntimeError("timed out waiting for a synchronized stereo pair")
            capture_realtime_ns = (
                sensor_to_realtime_ns(pair.left.sensor_timestamp_ns)
                if sensor_to_realtime_ns is not None
                else None
            )
            record("pair_wait_ms", stage_started_ns, measured)
            if measured:
                sensor_deltas_ns.append(pair.sensor_delta_ns)
                callback_deltas_ns.append(pair.callback_delta_ns)
                samples.setdefault("pair_ready_left_frame_age_ms", []).append(
                    (pair.ready_monotonic_ns - pair.left.sensor_timestamp_ns)
                    / 1_000_000.0
                )
                samples.setdefault("pair_ready_right_frame_age_ms", []).append(
                    (pair.ready_monotonic_ns - pair.right.sensor_timestamp_ns)
                    / 1_000_000.0
                )
                left_gain = float(pair.left.metadata.get("AnalogueGain", 0.0))
                right_gain = float(pair.right.metadata.get("AnalogueGain", 0.0))
                left_applied_gains.append(left_gain)
                right_applied_gains.append(right_gain)
                applied_gain_deltas.append(abs(left_gain - right_gain))

            try:
                left_color: np.ndarray | None = None
                stage_started_ns = time.perf_counter_ns()
                with pair.mapped_main() as (left_raw, right_raw):
                    record("camera_buffer_map_ms", stage_started_ns, measured)

                    stage_started_ns = time.perf_counter_ns()
                    left_gray_raw = cv2.cvtColor(left_raw, cv2.COLOR_BGR2GRAY)
                    right_gray_raw = cv2.cvtColor(
                        right_raw, cv2.COLOR_BGR2GRAY
                    )
                    record(
                        "grayscale_before_rectification_ms",
                        stage_started_ns,
                        measured,
                    )

                    stage_started_ns = time.perf_counter_ns()
                    left_gray = cv2.remap(
                        left_gray_raw,
                        left_maps[0],
                        left_maps[1],
                        cv2.INTER_LINEAR,
                    )
                    right_gray = cv2.remap(
                        right_gray_raw,
                        right_maps[0],
                        right_maps[1],
                        cv2.INTER_LINEAR,
                    )
                    record(
                        "grayscale_rectification_ms",
                        stage_started_ns,
                        measured,
                    )

                    if jpeg_due or semantic_due:
                        stage_started_ns = time.perf_counter_ns()
                        left_color = cv2.remap(
                            left_raw,
                            left_maps[0],
                            left_maps[1],
                            cv2.INTER_LINEAR,
                        )
                        record(
                            "throttled_color_rectification_ms",
                            stage_started_ns,
                            True,
                        )

                    # Do not retain exported DMABUF views beyond MappedArray.
                    del left_raw, right_raw, left_gray_raw, right_gray_raw
            finally:
                # Rectification owns its output, so camera DMABUFs can return to
                # libcamera before matching and publication.
                pair.close()

            stage_started_ns = time.perf_counter_ns()
            if args.working_width == WIDTH and args.working_height == HEIGHT:
                left_working = left_gray
                right_working = right_gray
            else:
                left_working = cv2.resize(
                    left_gray,
                    (args.working_width, args.working_height),
                    interpolation=cv2.INTER_AREA,
                )
                right_working = cv2.resize(
                    right_gray,
                    (args.working_width, args.working_height),
                    interpolation=cv2.INTER_AREA,
                )
            record("downscale_ms", stage_started_ns, measured)

            if auto_exposure is not None:
                stage_started_ns = time.perf_counter_ns()
                exposure_command = auto_exposure.observe(
                    left_working, right_working
                )
                if exposure_command is not None:
                    capture.set_exposure(
                        exposure_command.shutter_us,
                        exposure_command.gain,
                    )
                record("auto_exposure_ms", stage_started_ns, measured)

            stage_started_ns = time.perf_counter_ns()
            disparity_fixed = matcher.compute(left_working, right_working)
            record("sgbm_match_ms", stage_started_ns, measured)

            stage_started_ns = time.perf_counter_ns()
            valid_working = disparity_fixed > args.min_disparity * 16
            disparity_working = disparity_fixed.astype(np.float32) / 16.0
            disparity = cv2.resize(
                disparity_working,
                (WIDTH, HEIGHT),
                interpolation=cv2.INTER_LINEAR,
            )
            disparity *= WIDTH / args.working_width
            valid = cv2.resize(
                valid_working.astype(np.uint8),
                (WIDTH, HEIGHT),
                interpolation=cv2.INTER_NEAREST,
            ).astype(bool)
            valid &= np.isfinite(disparity)
            valid &= disparity > 0.0
            valid &= disparity < args.full_disparity_limit
            disparity[~valid] = np.nan
            if measured:
                valid_fractions.append(float(valid.mean()))
            record("disparity_restore_and_mask_ms", stage_started_ns, measured)

            depth: np.ndarray | None = None
            if args.metric_depth:
                stage_started_ns = time.perf_counter_ns()
                depth = np.full(disparity.shape, np.nan, dtype=np.float32)
                np.divide(
                    FOCAL_LENGTH_PX * BASELINE_M,
                    disparity,
                    out=depth,
                    where=valid,
                )
                record("metric_depth_ms", stage_started_ns, measured)

            jpeg_payload: np.ndarray | None = None
            if jpeg_due:
                if left_color is None:
                    raise RuntimeError("scheduled color frame was not rectified")
                stage_started_ns = time.perf_counter_ns()
                encoded, jpeg = cv2.imencode(
                    ".jpg",
                    left_color,
                    [cv2.IMWRITE_JPEG_QUALITY, args.jpeg_quality],
                )
                if not encoded:
                    raise RuntimeError("JPEG encoding failed")
                jpeg_sizes.append(int(jpeg.size))
                last_jpeg = jpeg
                jpeg_payload = jpeg
                record("jpeg_encode_ms", stage_started_ns, True)

            if native_output is not None:
                if capture_realtime_ns is None:
                    raise RuntimeError("native output capture stamp is missing")
                stage_started_ns = time.perf_counter_ns()
                last_native_output = native_output.publish(
                    capture_realtime_ns=capture_realtime_ns,
                    disparity=disparity,
                    left_rectified_mono=(
                        left_gray if args.publish_left_mono_to_ring else None
                    ),
                    left_rectified_jpeg=jpeg_payload,
                )
                record(
                    "native_cloud_and_result_ring_ms",
                    stage_started_ns,
                    measured,
                )
                if measured:
                    samples.setdefault("native_obstacle_cloud_ms", []).append(
                        float(last_native_output["obstacle_cloud_ms"])
                    )
                    samples.setdefault("native_result_ring_publish_ms", []).append(
                        float(last_native_output["result_ring_publish_ms"])
                    )

            if semantic_due:
                if semantic_worker is None or left_color is None:
                    raise RuntimeError("scheduled semantic color frame is missing")
                if capture_realtime_ns is None or last_native_output is None:
                    raise RuntimeError("semantic source sequence/stamp is missing")
                stage_started_ns = time.perf_counter_ns()
                if not semantic_worker.submit(
                    source_sequence=int(last_native_output["sequence"]),
                    capture_realtime_ns=capture_realtime_ns,
                    color_bgr=left_color,
                    disparity=disparity,
                    submitted_ns=semantic_schedule_ns,
                ):
                    raise RuntimeError("semantic worker rejected a scheduled frame")
                record("semantic_nonblocking_submit_ms", stage_started_ns, measured)

            stage_started_ns = time.perf_counter_ns()
            published_disparity = disparity.copy()
            published_depth = depth.copy() if depth is not None else None
            record("output_buffer_copy_ms", stage_started_ns, measured)
            if measured:
                output_ready_ns = time.monotonic_ns()
                samples.setdefault("depth_ready_left_frame_age_ms", []).append(
                    (output_ready_ns - pair.left.sensor_timestamp_ns)
                    / 1_000_000.0
                )
                samples.setdefault("depth_ready_right_frame_age_ms", []).append(
                    (output_ready_ns - pair.right.sensor_timestamp_ns)
                    / 1_000_000.0
                )
                samples.setdefault("cycle_ms", []).append(
                    milliseconds(cycle_started_ns)
                )
                last = {
                    "left": left_gray,
                    "right": right_gray,
                    "disparity": published_disparity,
                }
                if published_depth is not None:
                    last["depth"] = published_depth

        measured_wall_s = (
            time.perf_counter_ns() - measured_wall_started_ns
        ) / 1_000_000_000.0
        measured_process_cpu_s = (
            time.process_time() - measured_process_cpu_started
        )
        measured_system_ticks_finished = system_cpu_ticks()
        capture_stats = capture.stats()
    finally:
        capture.stop()
        if semantic_worker is not None:
            semantic_worker.close()
        if native_output is not None:
            native_output.close()

    process_cpu_percent = 100.0 * measured_process_cpu_s / measured_wall_s
    system_total_delta = (
        measured_system_ticks_finished[0] - measured_system_ticks_started[0]
    )
    system_idle_delta = (
        measured_system_ticks_finished[1] - measured_system_ticks_started[1]
    )
    report = {
        "configuration": {
            "input": [WIDTH, HEIGHT],
            "working": [args.working_width, args.working_height],
            "working_resize_applied": not (
                args.working_width == WIDTH and args.working_height == HEIGHT
            ),
            "working_disparities": args.working_disparities,
            "min_disparity": args.min_disparity,
            "full_disparity_limit": args.full_disparity_limit,
            "opencv_threads": cv2.getNumThreads(),
            "frames": args.frames,
            "warmup_frames": args.warmup_frames,
            "capture_fps": args.fps,
            "trigger_low_ms": args.trigger_low_ms,
            "initial_analogue_gain": args.analogue_gain,
            "fixed_colour_gains": list(args.colour_gains),
            "auto_exposure_enabled": args.auto_exposure,
            "pair_tolerance_us": args.pair_tolerance_us,
            "camera_input_copy_count": 0,
            "depth_rectification_input": "grayscale",
            "color_rectification_hz": args.jpeg_hz,
            "metric_depth_enabled": args.metric_depth,
            "block_size": args.block_size,
            "uniqueness_ratio": args.uniqueness_ratio,
            "speckle_window_size": args.speckle_window_size,
            "speckle_range": args.speckle_range,
            "disp12_max_diff": args.disp12_max_diff,
            "native_result_ring_enabled": native_output is not None,
            "native_result_ring_path": (
                str(args.result_ring) if args.result_ring is not None else None
            ),
            "native_result_ring_left_mono": args.publish_left_mono_to_ring,
            "semantic_enabled": args.semantic,
            "semantic_ring_path": str(args.semantic_ring) if args.semantic else None,
            "semantic_yolo_hz": args.semantic_yolo_hz,
            "semantic_segformer_hz": args.semantic_segformer_hz,
            "semantic_preview_enabled": args.semantic_preview,
        },
        "capture_start_ms": capture_start_ms,
        "jpeg_warmup_ms": jpeg_warmup_ms,
        "steady_state": {
            "wall_seconds": measured_wall_s,
            "achieved_hz": args.frames / measured_wall_s,
            "process_cpu_percent_one_core_100": process_cpu_percent,
            "soc_cpu_percent_four_core_100": process_cpu_percent / 4.0,
            "whole_soc_cpu_percent": 100.0
            * (system_total_delta - system_idle_delta)
            / system_total_delta,
            "stages": summaries(samples),
            "sensor_delta_ns": {
                "p50": percentile([float(value) for value in sensor_deltas_ns], 50),
                "p95": percentile([float(value) for value in sensor_deltas_ns], 95),
                "max": max(sensor_deltas_ns),
            },
            "callback_delta_ms": {
                "p50": percentile(
                    [value / 1_000_000.0 for value in callback_deltas_ns], 50
                ),
                "p95": percentile(
                    [value / 1_000_000.0 for value in callback_deltas_ns], 95
                ),
            },
            "jpeg_samples": len(jpeg_sizes),
            "jpeg_mean_bytes": float(np.mean(jpeg_sizes)) if jpeg_sizes else 0.0,
            "valid_fraction": float(
                np.isfinite(last["disparity"]).mean()
            ),
            "valid_fraction_distribution": {
                "min": min(valid_fractions),
                "p50": percentile(valid_fractions, 50),
                "p95": percentile(valid_fractions, 95),
                "max": max(valid_fractions),
            },
            "max_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
            "async_capture": capture_stats,
            "exposure_state": capture.exposure_state(),
            "auto_exposure": (
                auto_exposure.report() if auto_exposure is not None else None
            ),
            "applied_gain": {
                "left_p50": percentile(left_applied_gains, 50),
                "right_p50": percentile(right_applied_gains, 50),
                "pair_delta_max": max(applied_gain_deltas, default=0.0),
            },
            "native_output_last": last_native_output,
            "semantic": (
                semantic_worker.stats() if semantic_worker is not None else None
            ),
        },
    }

    cv2.imwrite(str(args.output_dir / "left_rect.png"), last["left"])
    cv2.imwrite(str(args.output_dir / "right_rect.png"), last["right"])
    if last_jpeg is not None:
        (args.output_dir / "left_rect_color.jpg").write_bytes(
            last_jpeg.tobytes()
        )
    np.save(args.output_dir / "disparity_float32.npy", last["disparity"])
    if "depth" in last:
        np.save(args.output_dir / "depth_m_float32.npy", last["depth"])
    (args.output_dir / "async_zero_copy_profile.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
