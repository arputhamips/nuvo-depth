#!/usr/bin/env python3
"""Replay one rectified pair through OpenCV StereoBM and record metrics."""

from __future__ import annotations

import argparse
import json
import os
import time
from pathlib import Path

import cv2
import numpy as np


def percentile(values: list[float], quantile: float) -> float:
    return float(np.percentile(np.asarray(values), quantile))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--left", type=Path, required=True)
    parser.add_argument("--right", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--target-hz", type=float, default=10.0)
    parser.add_argument("--opencv-threads", type=int, default=1)
    parser.add_argument("--min-disparity", type=int, default=0)
    parser.add_argument("--num-disparities", type=int, default=160)
    parser.add_argument("--full-disparity-limit", type=float, default=160.0)
    parser.add_argument("--block-size", type=int, default=5)
    parser.add_argument("--uniqueness-ratio", type=int, default=1)
    parser.add_argument("--speckle-window-size", type=int, default=30)
    parser.add_argument("--speckle-range", type=int, default=2)
    parser.add_argument("--disp12-max-diff", type=int, default=1)
    parser.add_argument("--texture-threshold", type=int, default=0)
    args = parser.parse_args()
    if args.iterations < 1 or args.warmup < 0 or args.target_hz <= 0.0:
        parser.error("invalid iteration count or target rate")
    if (
        args.num_disparities < 16
        or args.num_disparities % 16 != 0
        or args.block_size < 5
        or args.block_size % 2 == 0
        or args.uniqueness_ratio < 0
        or args.speckle_window_size < 0
        or args.speckle_range < 0
        or args.texture_threshold < 0
    ):
        parser.error("invalid StereoBM configuration")

    left = cv2.imread(str(args.left), cv2.IMREAD_GRAYSCALE)
    right = cv2.imread(str(args.right), cv2.IMREAD_GRAYSCALE)
    if left is None or right is None or left.shape != right.shape:
        raise RuntimeError("could not read an equal-size rectified pair")
    cv2.setNumThreads(args.opencv_threads)
    matcher = cv2.StereoBM_create(args.num_disparities, args.block_size)
    matcher.setMinDisparity(args.min_disparity)
    matcher.setPreFilterCap(31)
    matcher.setTextureThreshold(args.texture_threshold)
    matcher.setUniquenessRatio(args.uniqueness_ratio)
    matcher.setSpeckleWindowSize(args.speckle_window_size)
    matcher.setSpeckleRange(args.speckle_range)
    matcher.setDisp12MaxDiff(args.disp12_max_diff)

    for _ in range(args.warmup):
        matcher.compute(left, right)

    match_ms: list[float] = []
    postprocess_ms: list[float] = []
    total_ms: list[float] = []
    period_s = 1.0 / args.target_hz
    wall_started = time.perf_counter()
    process_started = time.process_time()
    next_frame = wall_started
    deadline_misses = 0
    disparity = np.empty(left.shape, dtype=np.float32)
    valid = np.empty(left.shape, dtype=bool)
    for _ in range(args.iterations):
        total_started = time.perf_counter_ns()
        stage_started = time.perf_counter_ns()
        disparity_fixed = matcher.compute(left, right)
        matched = time.perf_counter_ns()
        match_ms.append((matched - stage_started) / 1_000_000.0)

        disparity = disparity_fixed.astype(np.float32) / 16.0
        valid = disparity_fixed > args.min_disparity * 16
        valid &= np.isfinite(disparity)
        valid &= disparity < args.full_disparity_limit
        disparity[~valid] = np.nan
        finished = time.perf_counter_ns()
        postprocess_ms.append((finished - matched) / 1_000_000.0)
        total_ms.append((finished - total_started) / 1_000_000.0)

        next_frame += period_s
        remaining = next_frame - time.perf_counter()
        if remaining > 0.0:
            time.sleep(remaining)
        else:
            deadline_misses += 1

    wall_seconds = time.perf_counter() - wall_started
    process_seconds = time.process_time() - process_started
    logical_cpus = os.cpu_count() or 1
    searchable_start = min(
        left.shape[1] - 1,
        max(0, args.min_disparity + args.num_disparities),
    )
    searchable = valid[:, searchable_start:]

    args.output_dir.mkdir(parents=True, exist_ok=True)
    np.save(args.output_dir / "disparity_float32.npy", disparity)
    normalized = np.nan_to_num(
        disparity / args.full_disparity_limit * 255.0,
        nan=0.0,
        posinf=0.0,
        neginf=0.0,
    ).clip(0, 255).astype(np.uint8)
    color = cv2.applyColorMap(normalized, cv2.COLORMAP_TURBO)
    color[~valid] = 0
    cv2.imwrite(str(args.output_dir / "disparity_color.png"), color)
    cv2.imwrite(str(args.output_dir / "left_gray.png"), left)
    cv2.imwrite(str(args.output_dir / "right_gray.png"), right)

    process_cpu = 100.0 * process_seconds / wall_seconds
    report = {
        "backend": "opencv_stereo_bm_cpu",
        "input": [left.shape[1], left.shape[0]],
        "min_disparity": args.min_disparity,
        "num_disparities": args.num_disparities,
        "full_disparity_limit": args.full_disparity_limit,
        "block_size": args.block_size,
        "uniqueness_ratio": args.uniqueness_ratio,
        "speckle_window_size": args.speckle_window_size,
        "speckle_range": args.speckle_range,
        "disp12_max_diff": args.disp12_max_diff,
        "texture_threshold": args.texture_threshold,
        "opencv_threads": cv2.getNumThreads(),
        "target_hz": args.target_hz,
        "achieved_hz": args.iterations / wall_seconds,
        "deadline_misses": deadline_misses,
        "match_ms_p50": percentile(match_ms, 50),
        "match_ms_p95": percentile(match_ms, 95),
        "match_ms_p99": percentile(match_ms, 99),
        "postprocess_ms_p50": percentile(postprocess_ms, 50),
        "compute_ms_p50": percentile(total_ms, 50),
        "compute_ms_p95": percentile(total_ms, 95),
        "compute_ms_p99": percentile(total_ms, 99),
        "valid_fraction": float(valid.mean()),
        "searchable_valid_fraction": float(searchable.mean()),
        "process_cpu_percent_one_core_100": process_cpu,
        "soc_cpu_percent": process_cpu / logical_cpus,
        "logical_cpus": logical_cpus,
    }
    (args.output_dir / "metrics.json").write_text(
        json.dumps(report, indent=2) + "\n"
    )
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
