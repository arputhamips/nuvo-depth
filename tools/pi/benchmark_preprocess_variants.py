#!/usr/bin/env python3
"""Benchmark equivalent preprocessing layouts on one captured stereo pair."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
import cv2
import numpy as np

from capture_rectified_pair import calibration


INPUT_SIZE = (728, 544)
WORKING_SIZE = (364, 272)


def maps(path: Path, output_size: tuple[int, int]) -> tuple[np.ndarray, np.ndarray]:
    matrix, distortion, rectification, projection = calibration(path)
    new_matrix = projection[:, :3].copy()
    scale_x = output_size[0] / INPUT_SIZE[0]
    scale_y = output_size[1] / INPUT_SIZE[1]
    new_matrix[0, :] *= scale_x
    new_matrix[1, :] *= scale_y
    return cv2.initUndistortRectifyMap(
        matrix,
        distortion,
        rectification,
        new_matrix,
        output_size,
        cv2.CV_16SC2,
    )


def summary(values: list[float]) -> dict[str, float]:
    return {
        "p50_ms": float(np.percentile(values, 50)),
        "p95_ms": float(np.percentile(values, 95)),
        "max_ms": max(values),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--left-raw", type=Path, required=True)
    parser.add_argument("--right-raw", type=Path, required=True)
    parser.add_argument("--left-calibration", type=Path, required=True)
    parser.add_argument("--right-calibration", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--sgbm-iterations", type=int, default=50)
    args = parser.parse_args()
    cv2.setNumThreads(1)

    left_raw = cv2.imread(str(args.left_raw), cv2.IMREAD_COLOR)
    right_raw = cv2.imread(str(args.right_raw), cv2.IMREAD_COLOR)
    if left_raw is None or right_raw is None:
        raise RuntimeError("could not read raw stereo inputs")

    left_full_maps = maps(args.left_calibration, INPUT_SIZE)
    right_full_maps = maps(args.right_calibration, INPUT_SIZE)
    left_working_maps = maps(args.left_calibration, WORKING_SIZE)
    right_working_maps = maps(args.right_calibration, WORKING_SIZE)
    raw = (left_raw, right_raw)
    full_maps = (left_full_maps, right_full_maps)
    working_maps = (left_working_maps, right_working_maps)

    samples: dict[str, dict[str, list[float]]] = {
        "color_full_then_gray_resize": {},
        "gray_full_then_rectify_resize": {},
        "gray_then_direct_working_rectify": {},
        "throttled_color_output": {},
    }
    outputs: dict[str, tuple[np.ndarray, np.ndarray]] = {}

    for iteration in range(args.warmup + args.iterations):
        measured = iteration >= args.warmup
        local = {name: {} if measured else {} for name in samples}

        started = time.perf_counter_ns()
        rectified_color = [
            cv2.remap(image, pair[0], pair[1], cv2.INTER_LINEAR)
            for image, pair in zip(raw, full_maps)
        ]
        if measured:
            local["color_full_then_gray_resize"].setdefault(
                "rectify_two_color", []
            ).append((time.perf_counter_ns() - started) / 1_000_000.0)
        started = time.perf_counter_ns()
        gray = [cv2.cvtColor(image, cv2.COLOR_BGR2GRAY) for image in rectified_color]
        if measured:
            local["color_full_then_gray_resize"].setdefault("grayscale", []).append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )
        started = time.perf_counter_ns()
        current = [cv2.resize(image, WORKING_SIZE, interpolation=cv2.INTER_AREA) for image in gray]
        if measured:
            local["color_full_then_gray_resize"].setdefault("resize", []).append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )

        started = time.perf_counter_ns()
        raw_gray = [cv2.cvtColor(image, cv2.COLOR_BGR2GRAY) for image in raw]
        if measured:
            local["gray_full_then_rectify_resize"].setdefault("grayscale", []).append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )
        started = time.perf_counter_ns()
        rectified_gray = [
            cv2.remap(image, pair[0], pair[1], cv2.INTER_LINEAR)
            for image, pair in zip(raw_gray, full_maps)
        ]
        if measured:
            local["gray_full_then_rectify_resize"].setdefault(
                "rectify_two_gray", []
            ).append((time.perf_counter_ns() - started) / 1_000_000.0)
        started = time.perf_counter_ns()
        gray_first = [
            cv2.resize(image, WORKING_SIZE, interpolation=cv2.INTER_AREA)
            for image in rectified_gray
        ]
        if measured:
            local["gray_full_then_rectify_resize"].setdefault("resize", []).append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )

        started = time.perf_counter_ns()
        direct_gray = [cv2.cvtColor(image, cv2.COLOR_BGR2GRAY) for image in raw]
        if measured:
            local["gray_then_direct_working_rectify"].setdefault(
                "grayscale", []
            ).append((time.perf_counter_ns() - started) / 1_000_000.0)
        started = time.perf_counter_ns()
        direct = [
            cv2.remap(image, pair[0], pair[1], cv2.INTER_LINEAR)
            for image, pair in zip(direct_gray, working_maps)
        ]
        if measured:
            local["gray_then_direct_working_rectify"].setdefault(
                "direct_rectify_two_gray", []
            ).append((time.perf_counter_ns() - started) / 1_000_000.0)

        started = time.perf_counter_ns()
        output_left = cv2.remap(
            left_raw, left_full_maps[0], left_full_maps[1], cv2.INTER_LINEAR
        )
        if measured:
            local["throttled_color_output"].setdefault(
                "rectify_left_color", []
            ).append((time.perf_counter_ns() - started) / 1_000_000.0)
        started = time.perf_counter_ns()
        encoded, _ = cv2.imencode(".jpg", output_left, [cv2.IMWRITE_JPEG_QUALITY, 75])
        if not encoded:
            raise RuntimeError("JPEG encoding failed")
        if measured:
            local["throttled_color_output"].setdefault("jpeg", []).append(
                (time.perf_counter_ns() - started) / 1_000_000.0
            )

        if measured:
            for variant, stages in local.items():
                total = 0.0
                for stage, values in stages.items():
                    samples[variant].setdefault(stage, []).extend(values)
                    total += values[0]
                samples[variant].setdefault("total", []).append(total)
            outputs = {
                "color_full_then_gray_resize": (current[0], current[1]),
                "gray_full_then_rectify_resize": (gray_first[0], gray_first[1]),
                "gray_then_direct_working_rectify": (direct[0], direct[1]),
            }

    def matcher(mode: int) -> cv2.StereoSGBM:
        return cv2.StereoSGBM_create(
            0, 96, 5, 8 * 25, 32 * 25, 1, 31, 5, 50, 2, mode
        )

    quality_matcher = matcher(cv2.STEREO_SGBM_MODE_SGBM_3WAY)
    disparities = {
        name: quality_matcher.compute(pair[0], pair[1]).astype(np.float32) / 16.0
        for name, pair in outputs.items()
    }
    reference = disparities["color_full_then_gray_resize"]
    reference_valid = reference > 0
    quality = {}
    for name, disparity in disparities.items():
        valid = disparity > 0
        overlap = reference_valid & valid
        difference = np.abs(disparity[overlap] - reference[overlap])
        quality[name] = {
            "valid_fraction": float(valid.mean()),
            "reference_overlap_fraction": float(overlap.mean()),
            "median_abs_difference_working_px": float(np.median(difference)),
            "p95_abs_difference_working_px": float(np.percentile(difference, 95)),
        }

    sgbm_modes = {
        "sgbm": cv2.STEREO_SGBM_MODE_SGBM,
        "hh": cv2.STEREO_SGBM_MODE_HH,
        "sgbm_3way": cv2.STEREO_SGBM_MODE_SGBM_3WAY,
        "hh4": cv2.STEREO_SGBM_MODE_HH4,
    }
    sgbm_timings = {}
    sgbm_outputs = {}
    working_pair = outputs["color_full_then_gray_resize"]
    for name, mode in sgbm_modes.items():
        mode_matcher = matcher(mode)
        values = []
        result = None
        for iteration in range(args.warmup + args.sgbm_iterations):
            started = time.perf_counter_ns()
            result = mode_matcher.compute(working_pair[0], working_pair[1])
            elapsed = (time.perf_counter_ns() - started) / 1_000_000.0
            if iteration >= args.warmup:
                values.append(elapsed)
        sgbm_timings[name] = summary(values)
        sgbm_outputs[name] = result.astype(np.float32) / 16.0

    mode_reference = sgbm_outputs["sgbm_3way"]
    mode_reference_valid = mode_reference > 0
    sgbm_quality = {}
    for name, disparity in sgbm_outputs.items():
        valid = disparity > 0
        overlap = mode_reference_valid & valid
        difference = np.abs(disparity[overlap] - mode_reference[overlap])
        sgbm_quality[name] = {
            "valid_fraction": float(valid.mean()),
            "reference_overlap_fraction": float(overlap.mean()),
            "median_abs_difference_working_px": float(np.median(difference)),
            "p95_abs_difference_working_px": float(np.percentile(difference, 95)),
        }

    report = {
        "iterations": args.iterations,
        "opencv_threads": cv2.getNumThreads(),
        "timings": {
            variant: {stage: summary(values) for stage, values in stages.items()}
            for variant, stages in samples.items()
        },
        "disparity_quality_against_current": quality,
        "sgbm_mode_timings": sgbm_timings,
        "sgbm_mode_quality_against_3way": sgbm_quality,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
