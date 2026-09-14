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
from capture_stream_profile import (
    CAPTURE_STREAM_PROFILES,
    ISP_LUMA_RGB_LEFT_PROFILE,
)
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


def disparity_to_obstacle_voxels(disparity: np.ndarray) -> set[tuple[int, int, int]]:
    """Camera-model/rig-transform voxel proxy used by the A/B harness."""
    rows = np.arange(96, 420, 2)
    columns = np.arange(0, WIDTH, 2)
    u, v = np.meshgrid(columns, rows)
    values = disparity[v, u]
    valid = np.isfinite(values) & (values > 0.0)
    depth = np.full(values.shape, np.nan, dtype=np.float32)
    np.divide(FOCAL_LENGTH_PX * BASELINE_M, values, out=depth, where=valid)
    valid &= (depth >= 0.75) & (depth <= 6.0)
    optical_x = (u - 348.240810394) * depth / 493.542046188
    optical_y = (v - 266.509811401) * depth / 493.542046188
    base_x = depth + 0.760
    base_y = -optical_x + 0.050
    base_z = -optical_y + 0.180
    valid &= (base_z >= 0.18) & (base_z <= 1.20)
    valid &= base_x > 0.0
    planar = np.hypot(base_x, base_y)
    angle = np.arctan2(base_y, base_x)
    valid &= (planar >= 0.75) & (planar <= 6.0)
    valid &= (angle >= -0.70) & (angle <= 0.70)
    return set(
        zip(
            np.floor(base_x[valid] / 0.05).astype(np.int32).tolist(),
            np.floor(base_y[valid] / 0.05).astype(np.int32).tolist(),
            np.floor(base_z[valid] / 0.05).astype(np.int32).tolist(),
        )
    )


def histogram_percentile(histogram: np.ndarray, quantile: float) -> float:
    total = int(histogram.sum())
    if total == 0:
        return 0.0
    target = max(1, int(np.ceil(total * quantile / 100.0)))
    return float(np.searchsorted(np.cumsum(histogram), target))


def count_deadline_misses(
    sensor_timestamps_ns: list[int],
    *,
    fps: float,
    ready_pair_replaced: int,
) -> int:
    """Count actual skipped capture periods, not pacing-loop jitter."""
    period_ns = 1_000_000_000.0 / fps
    timestamp_misses = sum(
        max(0, int((current - previous) / period_ns + 0.5) - 1)
        for previous, current in zip(
            sensor_timestamps_ns, sensor_timestamps_ns[1:]
        )
    )
    # A replaced ready pair normally appears as the same timestamp gap. Taking
    # the maximum also covers a replacement at the measurement boundary without
    # double-counting it.
    return max(timestamp_misses, ready_pair_replaced)


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
        "--preview-source",
        choices=("rectified", "raw"),
        default="rectified",
        help="JPEG preview input; raw skips color rectification",
    )
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
    parser.add_argument(
        "--capture-stream-profile",
        choices=CAPTURE_STREAM_PROFILES,
        default=ISP_LUMA_RGB_LEFT_PROFILE,
        help=(
            "camera processed-stream layout (production uses ISP YUV luma "
            "for depth and a left-only RGB stream for scheduled color work)"
        ),
    )
    parser.add_argument(
        "--rectification-backend",
        choices=("cpu_opencv", "egl_gl_dmabuf"),
        default="cpu_opencv",
        help="depth rectifier; production default remains cpu_opencv",
    )
    parser.add_argument(
        "--rectification-fallback-backend",
        choices=("cpu_opencv",),
        default="cpu_opencv",
    )
    parser.add_argument(
        "--allow-rectification-fallback",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    parser.add_argument(
        "--egl-gl-rectification-library",
        type=Path,
        help="path to libdepthd_egl_gl_dmabuf_rectification.so",
    )
    parser.add_argument(
        "--compare-rectification-backends",
        action="store_true",
        help="run CPU remap on the same leased pairs and report pixel parity",
    )
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
    args = parser.parse_args()
    if (
        args.frames < 1
        or args.warmup_frames < 0
        or args.fps <= 0.0
        or args.jpeg_hz < 0.0
    ):
        parser.error("invalid frame counts or output rates")
    if (
        args.rectification_backend == "egl_gl_dmabuf"
        and args.capture_stream_profile != ISP_LUMA_RGB_LEFT_PROFILE
    ):
        parser.error("egl_gl_dmabuf requires isp_luma_rgb_left capture")
    if (
        args.rectification_backend == "egl_gl_dmabuf"
        and args.egl_gl_rectification_library is None
    ):
        parser.error("egl_gl_dmabuf requires --egl-gl-rectification-library")
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
        comparison_auto_exposure = (
            StereoAutoExposure(
                ae_config,
                initial_shutter_us=round(args.trigger_low_ms * 1000.0),
                initial_gain=args.analogue_gain,
            )
            if args.auto_exposure and args.compare_rectification_backends
            else None
        )
    except ValueError as error:
        parser.error(str(error))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    cv2.setNumThreads(1)

    left_maps = make_maps(args.left_calibration)
    right_maps = make_maps(args.right_calibration)
    egl_rectifier = None
    if args.rectification_backend == "egl_gl_dmabuf":
        from depthd_egl_gl_rectification import EglGlDmabufRectifier

        egl_rectifier = EglGlDmabufRectifier(
            left_maps,
            right_maps,
            library=args.egl_gl_rectification_library,
            cache_capacity=8,
            allow_cpu_fallback=args.allow_rectification_fallback,
        )
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
    comparison_matcher = (
        cv2.StereoSGBM_create(
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
        if args.compare_rectification_backends
        else None
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
        stream_profile=args.capture_stream_profile,
    )
    native_output = None
    comparison_native_output = None
    sensor_to_realtime_ns = None
    if args.result_ring is not None:
        from depthd_native_output import (
            NativeOutputPublisher,
            sensor_monotonic_to_realtime_ns,
        )

        try:
            native_output = NativeOutputPublisher(
                args.result_ring,
                args.native_output_library,
            )
            if args.compare_rectification_backends:
                comparison_native_output = NativeOutputPublisher(
                    args.output_dir / "comparison-cpu-results.ring",
                    args.native_output_library,
                )
        except Exception:
            if comparison_native_output is not None:
                comparison_native_output.close()
            if native_output is not None:
                native_output.close()
            raise
        sensor_to_realtime_ns = sensor_monotonic_to_realtime_ns
    samples: dict[str, list[float]] = {}
    sensor_deltas_ns: list[int] = []
    measured_sensor_timestamps_ns: list[int] = []
    callback_deltas_ns: list[int] = []
    jpeg_sizes: list[int] = []
    valid_fractions: list[float] = []
    left_applied_gains: list[float] = []
    right_applied_gains: list[float] = []
    applied_gain_deltas: list[float] = []
    left_applied_exposures_us: list[float] = []
    right_applied_exposures_us: list[float] = []
    applied_exposure_deltas_us: list[float] = []
    last_jpeg: np.ndarray | None = None
    last: dict[str, np.ndarray] = {}
    last_native_output: dict[str, float | int] | None = None
    last_comparison_native_output: dict[str, float | int] | None = None
    rectification_exact_pixels = 0
    rectification_within_one_pixels = 0
    rectification_compared_pixels = 0
    rectification_abs_error_sum = 0
    rectification_max_error = 0
    rectification_error_hist = np.zeros(256, dtype=np.int64)
    comparison_valid_agreement = 0
    comparison_valid_pixels = 0
    comparison_joint_valid = 0
    comparison_disparity_hist = np.zeros(
        args.working_disparities * 16 + 1, dtype=np.int64
    )
    comparison_cloud: dict[str, float | int] | None = None
    comparison_native_cloud: dict[str, dict[str, list[float]]] = {
        name: {"gpu": [], "cpu": [], "absolute_delta": []}
        for name in (
            "obstacle_point_count",
            "accepted_raw_points",
            "accumulated_voxels",
            "supported_voxels",
            "accepted_clusters",
        )
    }
    comparison_native_overflow = {"gpu": 0, "cpu": 0}
    comparison_ae_decisions = 0
    comparison_ae_mismatches = 0
    comparison_ae_max_shutter_delta_us = 0
    comparison_ae_max_gain_delta = 0.0
    egl_diagnostics: dict[str, object] | None = None
    jpeg_period = max(1, round(args.fps / max(args.jpeg_hz, 1e-6)))

    def record(name: str, started_ns: int, measured: bool) -> None:
        if measured:
            samples.setdefault(name, []).append(milliseconds(started_ns))

    def prepare_scheduled_color(
        color_view: np.ndarray,
        *,
        jpeg_due: bool,
        measured: bool,
    ) -> np.ndarray | None:
        """Own the scheduled preview color product."""
        preview_color = None
        rectified_color = None
        if jpeg_due and args.preview_source == "raw":
            stage_started_ns = time.perf_counter_ns()
            preview_color = color_view.copy()
            record("raw_preview_copy_ms", stage_started_ns, measured)
        if jpeg_due and args.preview_source == "rectified":
            stage_started_ns = time.perf_counter_ns()
            rectified_color = cv2.remap(
                color_view,
                left_maps[0],
                left_maps[1],
                cv2.INTER_LINEAR,
            )
            record(
                "throttled_color_rectification_ms",
                stage_started_ns,
                measured,
            )
        if jpeg_due and args.preview_source == "rectified":
            preview_color = rectified_color
        return preview_color

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
        if comparison_native_output is not None:
            comparison_native_output.close()
        if native_output is not None:
            native_output.close()
        if egl_rectifier is not None:
            egl_rectifier.close()
        raise
    capture_start_ms = milliseconds(capture_started_ns)
    try:
        total_frames = args.warmup_frames + args.frames
        for frame_index in range(total_frames):
            measured = frame_index >= args.warmup_frames
            jpeg_due = (
                args.jpeg_hz > 0.0
                and measured
                and (frame_index - args.warmup_frames) % jpeg_period == 0
            )
            if frame_index == args.warmup_frames:
                measured_wall_started_ns = time.perf_counter_ns()
                measured_process_cpu_started = time.process_time()
                measured_system_ticks_started = system_cpu_ticks()
            cycle_started_ns = time.perf_counter_ns()

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
                measured_sensor_timestamps_ns.append(
                    pair.left.sensor_timestamp_ns
                )
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
                left_exposure = float(
                    pair.left.metadata.get("ExposureTime", 0.0)
                )
                right_exposure = float(
                    pair.right.metadata.get("ExposureTime", 0.0)
                )
                left_applied_exposures_us.append(left_exposure)
                right_applied_exposures_us.append(right_exposure)
                applied_exposure_deltas_us.append(
                    abs(left_exposure - right_exposure)
                )

            try:
                preview_color: np.ndarray | None = None
                cpu_comparison: tuple[np.ndarray, np.ndarray] | None = None
                if egl_rectifier is not None:
                    dmabufs = pair.dmabuf_luma_views(
                        stream_profile=args.capture_stream_profile
                    )
                    stage_started_ns = time.perf_counter_ns()
                    try:
                        gpu_timing = egl_rectifier.rectify_pair(
                            dmabufs.left, dmabufs.right
                        )
                    except RuntimeError:
                        if not args.allow_rectification_fallback:
                            raise
                        # The native backend has switched permanently to its
                        # cpu_opencv fallback. Map the still-active requests
                        # only now, then retry this same accepted pair.
                        with pair.mapped_views(
                            stream_profile=args.capture_stream_profile,
                            need_left_color=jpeg_due,
                        ) as fallback_views:
                            gpu_timing = egl_rectifier.rectify_pair(
                                dmabufs.left,
                                dmabufs.right,
                                cpu_left=fallback_views.left_depth,
                                cpu_right=fallback_views.right_depth,
                            )
                            if jpeg_due:
                                if fallback_views.left_color is None:
                                    raise RuntimeError(
                                        "scheduled color work has no left color view"
                                    )
                                preview_color = prepare_scheduled_color(
                                    fallback_views.left_color,
                                    jpeg_due=jpeg_due,
                                    measured=measured,
                                )
                        del fallback_views
                    record("grayscale_rectification_ms", stage_started_ns, measured)
                    if measured:
                        for key, value in gpu_timing.items():
                            samples.setdefault(
                                f"egl_gl_rectification_{key}", []
                            ).append(value)
                    left_gray = egl_rectifier.left_output
                    right_gray = egl_rectifier.right_output

                    needs_mapping = (
                        args.compare_rectification_backends
                        or (jpeg_due and preview_color is None)
                    )
                    if needs_mapping:
                        stage_started_ns = time.perf_counter_ns()
                        with pair.mapped_views(
                            stream_profile=args.capture_stream_profile,
                            need_left_color=jpeg_due,
                        ) as views:
                            record("camera_buffer_map_ms", stage_started_ns, measured)
                            if args.compare_rectification_backends:
                                comparison_left = cv2.remap(
                                    views.left_depth,
                                    left_maps[0],
                                    left_maps[1],
                                    cv2.INTER_LINEAR,
                                )
                                comparison_right = cv2.remap(
                                    views.right_depth,
                                    right_maps[0],
                                    right_maps[1],
                                    cv2.INTER_LINEAR,
                                )
                                cpu_comparison = (
                                    comparison_left,
                                    comparison_right,
                                )
                                if measured:
                                    for gpu_image, cpu_image in (
                                        (left_gray, comparison_left),
                                        (right_gray, comparison_right),
                                    ):
                                        error = np.abs(
                                            gpu_image.astype(np.int16)
                                            - cpu_image.astype(np.int16)
                                        )
                                        rectification_compared_pixels += error.size
                                        rectification_exact_pixels += int(
                                            np.count_nonzero(error == 0)
                                        )
                                        rectification_within_one_pixels += int(
                                            np.count_nonzero(error <= 1)
                                        )
                                        rectification_abs_error_sum += int(error.sum())
                                        rectification_error_hist += np.bincount(
                                            error.ravel(), minlength=256
                                        )
                                        rectification_max_error = max(
                                            rectification_max_error,
                                            int(error.max()),
                                        )
                            if jpeg_due:
                                if views.left_color is None:
                                    raise RuntimeError(
                                        "scheduled color work has no left color view"
                                    )
                                preview_color = prepare_scheduled_color(
                                    views.left_color,
                                    jpeg_due=jpeg_due,
                                    measured=measured,
                                )
                        del views
                else:
                    stage_started_ns = time.perf_counter_ns()
                    with pair.mapped_views(
                        stream_profile=args.capture_stream_profile,
                        need_left_color=jpeg_due,
                    ) as views:
                        record("camera_buffer_map_ms", stage_started_ns, measured)
                        stage_started_ns = time.perf_counter_ns()
                        if views.depth_is_gray:
                            left_gray_raw = views.left_depth
                            right_gray_raw = views.right_depth
                        else:
                            left_gray_raw = cv2.cvtColor(
                                views.left_depth, cv2.COLOR_BGR2GRAY
                            )
                            right_gray_raw = cv2.cvtColor(
                                views.right_depth, cv2.COLOR_BGR2GRAY
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
                        if jpeg_due:
                            if views.left_color is None:
                                raise RuntimeError(
                                    "scheduled color work has no left color view"
                                )
                            preview_color = prepare_scheduled_color(
                                views.left_color,
                                jpeg_due=jpeg_due,
                                measured=measured,
                            )
                    del views
                    del left_gray_raw, right_gray_raw
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

            comparison_left_working: np.ndarray | None = None
            comparison_right_working: np.ndarray | None = None
            if cpu_comparison is not None:
                comparison_left_working, comparison_right_working = (
                    cpu_comparison
                )
                if args.working_width != WIDTH or args.working_height != HEIGHT:
                    comparison_left_working = cv2.resize(
                        comparison_left_working,
                        (args.working_width, args.working_height),
                        interpolation=cv2.INTER_AREA,
                    )
                    comparison_right_working = cv2.resize(
                        comparison_right_working,
                        (args.working_width, args.working_height),
                        interpolation=cv2.INTER_AREA,
                    )

            if auto_exposure is not None:
                stage_started_ns = time.perf_counter_ns()
                exposure_command = auto_exposure.observe(
                    left_working, right_working
                )
                comparison_exposure_command = None
                if comparison_auto_exposure is not None:
                    if (
                        comparison_left_working is None
                        or comparison_right_working is None
                    ):
                        raise RuntimeError("CPU AE comparison images are missing")
                    comparison_exposure_command = (
                        comparison_auto_exposure.observe(
                            comparison_left_working,
                            comparison_right_working,
                        )
                    )
                    if measured:
                        comparison_ae_decisions += 1
                        controls_match = (
                            exposure_command is None
                            and comparison_exposure_command is None
                        ) or (
                            exposure_command is not None
                            and comparison_exposure_command is not None
                            and exposure_command.shutter_us
                            == comparison_exposure_command.shutter_us
                            and exposure_command.gain
                            == comparison_exposure_command.gain
                            and exposure_command.reason
                            == comparison_exposure_command.reason
                        )
                        if not controls_match:
                            comparison_ae_mismatches += 1
                        if (
                            exposure_command is not None
                            and comparison_exposure_command is not None
                        ):
                            comparison_ae_max_shutter_delta_us = max(
                                comparison_ae_max_shutter_delta_us,
                                abs(
                                    exposure_command.shutter_us
                                    - comparison_exposure_command.shutter_us
                                ),
                            )
                            comparison_ae_max_gain_delta = max(
                                comparison_ae_max_gain_delta,
                                abs(
                                    exposure_command.gain
                                    - comparison_exposure_command.gain
                                ),
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

            comparison_disparity_fixed: np.ndarray | None = None
            if cpu_comparison is not None:
                if (
                    comparison_left_working is None
                    or comparison_right_working is None
                ):
                    raise RuntimeError("CPU disparity comparison images are missing")
                stage_started_ns = time.perf_counter_ns()
                comparison_disparity_fixed = comparison_matcher.compute(
                    comparison_left_working, comparison_right_working
                )
                record("comparison_cpu_sgbm_ms", stage_started_ns, measured)
                if measured:
                    gpu_valid = disparity_fixed > args.min_disparity * 16
                    cpu_valid = (
                        comparison_disparity_fixed > args.min_disparity * 16
                    )
                    comparison_valid_pixels += gpu_valid.size
                    comparison_valid_agreement += int(
                        np.count_nonzero(gpu_valid == cpu_valid)
                    )
                    joint = gpu_valid & cpu_valid
                    comparison_joint_valid += int(np.count_nonzero(joint))
                    fixed_difference = np.abs(
                        disparity_fixed[joint].astype(np.int32)
                        - comparison_disparity_fixed[joint].astype(np.int32)
                    )
                    if fixed_difference.size:
                        fixed_difference = np.minimum(
                            fixed_difference,
                            comparison_disparity_hist.size - 1,
                        )
                        comparison_disparity_hist += np.bincount(
                            fixed_difference,
                            minlength=comparison_disparity_hist.size,
                        )

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

            comparison_disparity: np.ndarray | None = None
            if measured and comparison_disparity_fixed is not None:
                comparison_valid_working = (
                    comparison_disparity_fixed > args.min_disparity * 16
                )
                comparison_working = (
                    comparison_disparity_fixed.astype(np.float32) / 16.0
                )
                comparison_disparity = cv2.resize(
                    comparison_working,
                    (WIDTH, HEIGHT),
                    interpolation=cv2.INTER_LINEAR,
                )
                comparison_disparity *= WIDTH / args.working_width
                comparison_valid = cv2.resize(
                    comparison_valid_working.astype(np.uint8),
                    (WIDTH, HEIGHT),
                    interpolation=cv2.INTER_NEAREST,
                ).astype(bool)
                comparison_valid &= np.isfinite(comparison_disparity)
                comparison_valid &= comparison_disparity > 0.0
                comparison_valid &= (
                    comparison_disparity < args.full_disparity_limit
                )
                comparison_disparity[~comparison_valid] = np.nan
                gpu_voxels = disparity_to_obstacle_voxels(disparity)
                cpu_voxels = disparity_to_obstacle_voxels(comparison_disparity)
                union = gpu_voxels | cpu_voxels
                comparison_cloud = {
                    "gpu_camera_frame_voxels": len(gpu_voxels),
                    "cpu_camera_frame_voxels": len(cpu_voxels),
                    "voxel_jaccard": (
                        len(gpu_voxels & cpu_voxels) / len(union)
                        if union
                        else 1.0
                    ),
                    "note": (
                        "5 cm pre-support spatial proxy; native compact-cloud "
                        "counts remain in native_output_last"
                    ),
                }

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
                if preview_color is None:
                    raise RuntimeError("scheduled preview color frame is missing")
                stage_started_ns = time.perf_counter_ns()
                encoded, jpeg = cv2.imencode(
                    ".jpg",
                    preview_color,
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

            if comparison_native_output is not None and measured:
                if capture_realtime_ns is None or comparison_disparity is None:
                    raise RuntimeError(
                        "native cloud comparison disparity/stamp is missing"
                    )
                last_comparison_native_output = comparison_native_output.publish(
                    capture_realtime_ns=capture_realtime_ns,
                    disparity=comparison_disparity,
                )
                if last_native_output is None:
                    raise RuntimeError("selected native cloud output is missing")
                for name, values in comparison_native_cloud.items():
                    gpu_value = float(last_native_output[name])
                    cpu_value = float(last_comparison_native_output[name])
                    values["gpu"].append(gpu_value)
                    values["cpu"].append(cpu_value)
                    values["absolute_delta"].append(
                        abs(gpu_value - cpu_value)
                    )
                comparison_native_overflow["gpu"] += int(
                    last_native_output["voxel_capacity_overflow"]
                )
                comparison_native_overflow["cpu"] += int(
                    last_comparison_native_output["voxel_capacity_overflow"]
                )

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
        if egl_rectifier is not None:
            egl_diagnostics = egl_rectifier.diagnostics()
    finally:
        capture.stop()
        if native_output is not None:
            native_output.close()
        if comparison_native_output is not None:
            comparison_native_output.close()
        if egl_rectifier is not None:
            egl_rectifier.close()

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
            "preview_source": args.preview_source,
            "trigger_low_ms": args.trigger_low_ms,
            "initial_analogue_gain": args.analogue_gain,
            "fixed_colour_gains": list(args.colour_gains),
            "auto_exposure_enabled": args.auto_exposure,
            "pair_tolerance_us": args.pair_tolerance_us,
            "camera_input_copy_count": 0,
            "capture_stream_profile": args.capture_stream_profile,
            "rectification_backend": args.rectification_backend,
            "rectification_fallback_backend": (
                args.rectification_fallback_backend
            ),
            "allow_rectification_fallback": (
                args.allow_rectification_fallback
            ),
            "compare_rectification_backends": (
                args.compare_rectification_backends
            ),
            "depth_rectification_input": "grayscale",
            "color_rectification_hz": (
                args.jpeg_hz if args.preview_source == "rectified" else 0.0
            ),
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
            "rectification_backend_diagnostics": egl_diagnostics,
            "rectification_comparison": (
                {
                    "pixel_equal_fraction": (
                        rectification_exact_pixels
                        / rectification_compared_pixels
                    ),
                    "pixel_within_one_fraction": (
                        rectification_within_one_pixels
                        / rectification_compared_pixels
                    ),
                    "pixel_mae_lsb": (
                        rectification_abs_error_sum
                        / rectification_compared_pixels
                    ),
                    "pixel_p95_lsb": histogram_percentile(
                        rectification_error_hist, 95
                    ),
                    "pixel_max_lsb": rectification_max_error,
                    "disparity_valid_mask_agreement": (
                        comparison_valid_agreement / comparison_valid_pixels
                    ),
                    "disparity_joint_valid_pixels": comparison_joint_valid,
                    "disparity_difference_working_px": {
                        "p50": histogram_percentile(
                            comparison_disparity_hist, 50
                        )
                        / 16.0,
                        "p95": histogram_percentile(
                            comparison_disparity_hist, 95
                        )
                        / 16.0,
                        "max": float(
                            np.flatnonzero(comparison_disparity_hist)[-1]
                        )
                        / 16.0
                        if comparison_disparity_hist.any()
                        else 0.0,
                    },
                    "compact_cloud_spatial_proxy_last": comparison_cloud,
                    "auto_exposure_same_pair": {
                        "decisions_compared": comparison_ae_decisions,
                        "control_mismatches": comparison_ae_mismatches,
                        "max_shutter_delta_us": (
                            comparison_ae_max_shutter_delta_us
                        ),
                        "max_gain_delta": comparison_ae_max_gain_delta,
                        "gpu_controller": (
                            auto_exposure.report()
                            if auto_exposure is not None
                            else None
                        ),
                        "cpu_controller": (
                            comparison_auto_exposure.report()
                            if comparison_auto_exposure is not None
                            else None
                        ),
                    },
                    "native_compact_cloud_same_pair": {
                        "frames": len(
                            comparison_native_cloud[
                                "obstacle_point_count"
                            ]["gpu"]
                        ),
                        "counts": {
                            name: {
                                "gpu_p50": percentile(values["gpu"], 50),
                                "cpu_p50": percentile(values["cpu"], 50),
                                "absolute_delta_p50": percentile(
                                    values["absolute_delta"], 50
                                ),
                                "absolute_delta_p95": percentile(
                                    values["absolute_delta"], 95
                                ),
                                "absolute_delta_max": max(
                                    values["absolute_delta"], default=0.0
                                ),
                            }
                            for name, values in comparison_native_cloud.items()
                        },
                        "voxel_capacity_overflow": comparison_native_overflow,
                        "cpu_last": last_comparison_native_output,
                    },
                }
                if rectification_compared_pixels > 0
                and comparison_valid_pixels > 0
                else None
            ),
            "deadline_misses": count_deadline_misses(
                measured_sensor_timestamps_ns,
                fps=args.fps,
                ready_pair_replaced=int(
                    capture_stats.get("ready_pair_replaced", 0)
                ),
            ),
            "cycle_over_period_samples": sum(
                value > 1000.0 / args.fps
                for value in samples.get("cycle_ms", [])
            ),
            "exposure_state": capture.exposure_state(),
            "auto_exposure": (
                auto_exposure.report() if auto_exposure is not None else None
            ),
            "applied_gain": {
                "left_p50": percentile(left_applied_gains, 50),
                "right_p50": percentile(right_applied_gains, 50),
                "pair_delta_max": max(applied_gain_deltas, default=0.0),
            },
            "applied_exposure_us": {
                "left_p50": percentile(left_applied_exposures_us, 50),
                "right_p50": percentile(right_applied_exposures_us, 50),
                "pair_delta_max": max(
                    applied_exposure_deltas_us, default=0.0
                ),
            },
            "native_output_last": last_native_output,
        },
    }

    cv2.imwrite(str(args.output_dir / "left_rect.png"), last["left"])
    cv2.imwrite(str(args.output_dir / "right_rect.png"), last["right"])
    if last_jpeg is not None:
        preview_filename = (
            "left_raw_preview.jpg"
            if args.preview_source == "raw"
            else "left_rect_color.jpg"
        )
        (args.output_dir / preview_filename).write_bytes(last_jpeg.tobytes())
    np.save(args.output_dir / "disparity_float32.npy", last["disparity"])
    if "depth" in last:
        np.save(args.output_dir / "depth_m_float32.npy", last["depth"])
    (args.output_dir / "async_zero_copy_profile.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
