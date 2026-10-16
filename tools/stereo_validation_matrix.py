#!/usr/bin/env python3
"""Run a production-matcher matrix on Sintel and retained Pi stereo imagery."""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
from dataclasses import dataclass, replace
from pathlib import Path

import cv2
import numpy as np


@dataclass(frozen=True)
class Profile:
    name: str
    algorithm: str
    min_disparity: int = 0
    search_range: int = 96
    max_disparity: int = 96
    block_size: int = 7
    sgbm_mode: str = "3way"
    uniqueness_ratio: int = 1
    speckle_window_size: int = 30
    speckle_range: int = 2
    disparity_12_max_diff: int = 1
    pre_filter_cap: int = 31
    bm_pre_filter_size: int = 9
    bm_texture_threshold: int = 10
    specular_threshold: int = 256
    median_filter_size: int = 0
    opencv_threads: int = 4

    def control(self) -> dict[str, int | str]:
        value = dict(self.__dict__)
        value.pop("name")
        return {"schema_version": 1, **value}


def profile_matrix(scale: int = 1) -> list[Profile]:
    """Cover every exposed matcher setting plus the most useful interactions."""
    result: dict[tuple, Profile] = {}

    def add(profile: Profile) -> None:
        key = tuple(profile.control().items())
        result.setdefault(key, profile)

    if scale not in (1, 2):
        raise ValueError("matching scale must be 1 or 2")
    prefix = "" if scale == 1 else "half_"
    base_range = 96 if scale == 1 else 48
    sgbm = Profile(f"{prefix}sgbm_reference", "sgbm",
                   search_range=base_range, max_disparity=base_range,
                   block_size=7 if scale == 1 else 5)
    bm = Profile(f"{prefix}bm_reference", "bm",
                 search_range=base_range, max_disparity=base_range,
                 block_size=15 if scale == 1 else 9)
    add(sgbm)
    add(bm)
    one_factor = (
        ("range", "search_range", (64, 80, 112, 128) if scale == 1 else (32, 64)),
        ("block", "block_size", (5, 9, 11, 15, 21)),
        ("unique", "uniqueness_ratio", (0, 3, 5, 10, 15)),
        ("speckle_window", "speckle_window_size", (0, 20, 50, 100)),
        ("speckle_range", "speckle_range", (1, 4, 8)),
        ("lr", "disparity_12_max_diff", (-1, 0, 2, 4)),
        ("prefilter", "pre_filter_cap", (15, 63)),
        ("specular", "specular_threshold", (200, 230, 245)),
    )
    for base in (sgbm, bm):
        for label, field, values in one_factor:
            for value in values:
                if field == "block_size" and base.algorithm == "bm" and value < 5:
                    continue
                suffix = str(value).replace("-", "m")
                updates = {field: value}
                if field == "search_range":
                    updates["max_disparity"] = value
                add(replace(base, name=f"{prefix}{base.algorithm}_{label}_{suffix}", **updates))

    for value in ("standard", "hh4", "hh"):
        add(replace(sgbm, name=f"{prefix}sgbm_mode_{value}", sgbm_mode=value))
    for base in (sgbm, bm):
        for value in (3, 5):
            add(replace(base, name=f"{prefix}{base.algorithm}_median_{value}",
                        median_filter_size=value))

    for value in (5, 15, 25, 51):
        add(replace(bm, name=f"{prefix}bm_prefilter_size_{value}", bm_pre_filter_size=value))
    for value in (0, 5, 20, 50, 100):
        add(replace(bm, name=f"{prefix}bm_texture_{value}", bm_texture_threshold=value))

    # Important range/window/quality interactions around the product geometry.
    interaction_ranges = (80, 96, 112) if scale == 1 else (32, 48, 64)
    for search in interaction_ranges:
        for block in (5, 7, 9):
            for unique in (0, 3, 5):
                add(replace(sgbm,
                            name=f"{prefix}sgbm_r{search}_b{block}_u{unique}",
                            search_range=search, max_disparity=search,
                            block_size=block, uniqueness_ratio=unique))
        for block in (9, 15, 21):
            for texture in (0, 10, 20):
                add(replace(bm,
                            name=f"{prefix}bm_r{search}_b{block}_t{texture}",
                            search_range=search, max_disparity=search,
                            block_size=block, bm_texture_threshold=texture))
    return list(result.values())


def matcher(profile: Profile):
    if profile.algorithm == "bm":
        value = cv2.StereoBM_create(profile.search_range, profile.block_size)
        value.setMinDisparity(profile.min_disparity)
        value.setPreFilterSize(profile.bm_pre_filter_size)
        value.setPreFilterCap(profile.pre_filter_cap)
        value.setTextureThreshold(profile.bm_texture_threshold)
        value.setUniquenessRatio(profile.uniqueness_ratio)
        value.setSpeckleWindowSize(profile.speckle_window_size)
        value.setSpeckleRange(profile.speckle_range)
        value.setDisp12MaxDiff(profile.disparity_12_max_diff)
        return value
    area = profile.block_size * profile.block_size
    modes = {"3way": cv2.STEREO_SGBM_MODE_SGBM_3WAY,
             "standard": cv2.STEREO_SGBM_MODE_SGBM,
             "hh4": cv2.STEREO_SGBM_MODE_HH4,
             "hh": cv2.STEREO_SGBM_MODE_HH}
    return cv2.StereoSGBM_create(
        minDisparity=profile.min_disparity,
        numDisparities=profile.search_range,
        blockSize=profile.block_size,
        P1=8 * area,
        P2=32 * area,
        disp12MaxDiff=profile.disparity_12_max_diff,
        preFilterCap=profile.pre_filter_cap,
        uniquenessRatio=profile.uniqueness_ratio,
        speckleWindowSize=profile.speckle_window_size,
        speckleRange=profile.speckle_range,
        mode=modes[profile.sgbm_mode],
    )


def valid_disparity(profile: Profile, left: np.ndarray, right: np.ndarray):
    started = cv2.getTickCount()
    original_fixed = matcher(profile).compute(left, right)
    fixed = original_fixed
    if profile.median_filter_size:
        filtered = cv2.medianBlur(original_fixed, profile.median_filter_size)
        fixed = filtered
    disparity = fixed.astype(np.float32) / 16.0
    original_disparity = original_fixed.astype(np.float32) / 16.0
    elapsed_ms = (cv2.getTickCount() - started) * 1000.0 / cv2.getTickFrequency()
    x, y, width, height = cv2.getValidDisparityROI(
        (0, 0, left.shape[1], left.shape[0]),
        (0, 0, right.shape[1], right.shape[0]),
        profile.min_disparity, profile.search_range, profile.block_size,
    )
    roi = np.zeros(left.shape, dtype=bool)
    roi[y : y + height, x : x + width] = True
    valid = (roi & np.isfinite(original_disparity) &
             (original_disparity >= profile.min_disparity) &
             (original_disparity < profile.max_disparity) &
             (left < profile.specular_threshold))
    filtered_outside = ((disparity < profile.min_disparity) |
                        (disparity >= profile.max_disparity))
    disparity[valid & filtered_outside] = original_disparity[valid & filtered_outside]
    disparity[~valid] = np.nan
    return disparity, valid, roi, elapsed_ms


def invalid_aware_upscale(disparity: np.ndarray, valid: np.ndarray,
                          roi: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    filled = np.nan_to_num(disparity, nan=0.0)
    full = cv2.resize(filled, (728, 544), interpolation=cv2.INTER_LINEAR) * 2.0
    support = cv2.resize(valid.astype(np.float32), (728, 544),
                         interpolation=cv2.INTER_LINEAR)
    full_valid = support >= 0.999999
    full[~full_valid] = np.nan
    full_roi = cv2.resize(roi.astype(np.uint8), (728, 544),
                          interpolation=cv2.INTER_NEAREST) != 0
    return full, full_valid, full_roi


def camera_score(profile: Profile, left: np.ndarray, right: np.ndarray,
                 scale: int) -> tuple[dict, np.ndarray]:
    match_left = left if scale == 1 else cv2.resize(left, (364, 272),
                                                     interpolation=cv2.INTER_AREA)
    match_right = right if scale == 1 else cv2.resize(right, (364, 272),
                                                       interpolation=cv2.INTER_AREA)
    disparity, valid, roi, elapsed_ms = valid_disparity(profile, match_left, match_right)
    if scale == 2:
        disparity, valid, roi = invalid_aware_upscale(disparity, valid, roi)
    yy, xx = np.indices(left.shape, dtype=np.float32)
    sample_x = xx - np.nan_to_num(disparity, nan=0.0)
    warped = cv2.remap(right, sample_x, yy, cv2.INTER_LINEAR,
                       borderMode=cv2.BORDER_CONSTANT)
    usable = valid & (sample_x >= 0.0)
    residual = np.abs(left.astype(np.float32) - warped.astype(np.float32))[usable]
    count, _, stats, _ = cv2.connectedComponentsWithStats(valid.astype(np.uint8), 8)
    component_areas = stats[1:, cv2.CC_STAT_AREA] if count > 1 else np.array([])
    small_pixels = int(component_areas[component_areas < 100].sum())
    color_value = np.zeros(left.shape, np.uint8)
    color_value[valid] = np.clip(disparity[valid] * 255.0 / 128.0, 0, 255).astype(np.uint8)
    color = cv2.applyColorMap(color_value, cv2.COLORMAP_TURBO)
    color[~valid] = 0
    metrics = {
        "camera_valid_fraction": float(np.mean(valid)),
        "camera_searchable_valid_fraction": float(np.sum(valid) / max(1, np.sum(roi))),
        "camera_photometric_mae": float(np.mean(residual)) if residual.size else math.nan,
        "camera_photometric_p95": float(np.percentile(residual, 95)) if residual.size else math.nan,
        "camera_small_component_pixels": small_pixels,
        "host_camera_match_ms": elapsed_ms,
    }
    return metrics, color


def epipolar_metrics(left: np.ndarray, right: np.ndarray) -> dict:
    detector = cv2.SIFT_create(nfeatures=5000, contrastThreshold=0.01)
    left_keys, left_desc = detector.detectAndCompute(left, None)
    right_keys, right_desc = detector.detectAndCompute(right, None)
    if left_desc is None or right_desc is None or len(right_desc) < 2:
        raise RuntimeError("not enough stereo texture for epipolar scoring")
    pairs = cv2.BFMatcher().knnMatch(left_desc, right_desc, k=2)
    left_points, right_points = [], []
    for candidates in pairs:
        if len(candidates) != 2:
            continue
        first, second = candidates
        if first.distance >= 0.7 * second.distance:
            continue
        left_points.append(left_keys[first.queryIdx].pt)
        right_points.append(right_keys[first.trainIdx].pt)
    if len(left_points) < 12:
        raise RuntimeError("not enough reliable stereo features for epipolar scoring")
    left_points = np.asarray(left_points, np.float32)
    right_points = np.asarray(right_points, np.float32)
    # Do not prefilter by the disparity sign/range expected by the matcher:
    # that hides reversed stereo geometry and scores unrelated matches instead.
    # Reject feature outliers geometrically before measuring vertical error.
    fundamental, inliers = cv2.findFundamentalMat(
        left_points, right_points, cv2.FM_RANSAC, 1.0, 0.999)
    if fundamental is None or inliers is None or np.sum(inliers) < 12:
        raise RuntimeError("not enough geometric stereo inliers for epipolar scoring")
    geometric_inliers = inliers.reshape(-1).astype(bool)
    delta = left_points[geometric_inliers] - right_points[geometric_inliers]
    values = np.c_[right_points[geometric_inliers], delta[:, 1]].astype(np.float64)
    design = np.c_[values[:, 0], values[:, 1], np.ones(len(values))]
    keep = np.ones(len(values), dtype=bool)
    coefficients = np.zeros(3)
    for _ in range(5):
        coefficients = np.linalg.lstsq(design[keep], values[keep, 2], rcond=None)[0]
        residual = values[:, 2] - design @ coefficients
        center = np.median(residual[keep])
        mad = np.median(np.abs(residual[keep] - center)) + 1e-9
        keep = np.abs(residual - center) < 3.0 * 1.4826 * mad
    return {
        "feature_matches": int(len(left_points)),
        "geometric_inlier_matches": int(len(values)),
        "inlier_matches": int(np.sum(keep)),
        "horizontal_disparity_p05_px": float(np.percentile(delta[:, 0], 5)),
        "horizontal_disparity_p50_px": float(np.percentile(delta[:, 0], 50)),
        "horizontal_disparity_p95_px": float(np.percentile(delta[:, 0], 95)),
        "negative_disparity_fraction": float(np.mean(delta[:, 0] < 0)),
        "vertical_abs_p50_px": float(np.percentile(np.abs(values[:, 2]), 50)),
        "vertical_abs_p95_px": float(np.percentile(np.abs(values[:, 2]), 95)),
        "vertical_affine_dx": float(coefficients[0]),
        "vertical_affine_dy": float(coefficients[1]),
        "vertical_affine_offset": float(coefficients[2]),
        "affine_residual_abs_p95_px": float(np.percentile(np.abs(residual[keep]), 95)),
    }


def labelled(image: np.ndarray, profile: Profile, metrics: dict) -> np.ndarray:
    panel = cv2.copyMakeBorder(image, 50, 0, 0, 0, cv2.BORDER_CONSTANT,
                               value=(22, 24, 29))
    line1 = profile.name
    line2 = (f"valid {100*metrics['camera_valid_fraction']:.1f}%  "
             f"photo MAE {metrics['camera_photometric_mae']:.1f}  "
             f"{metrics['host_camera_match_ms']:.1f} ms")
    cv2.putText(panel, line1, (8, 19), cv2.FONT_HERSHEY_SIMPLEX, 0.48,
                (245, 245, 245), 1, cv2.LINE_AA)
    cv2.putText(panel, line2, (8, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.42,
                (165, 190, 255), 1, cv2.LINE_AA)
    return panel


def grids(profiles: list[Profile], images: dict[str, np.ndarray], output: Path) -> None:
    for algorithm in ("sgbm", "bm"):
        selected = [p for p in profiles if p.algorithm == algorithm]
        for page, start in enumerate(range(0, len(selected), 12), 1):
            group = selected[start : start + 12]
            thumbs = [cv2.resize(images[p.name], (364, 297), interpolation=cv2.INTER_AREA)
                      for p in group]
            while len(thumbs) < 12:
                thumbs.append(np.zeros_like(thumbs[0]))
            rows = [np.hstack(thumbs[index : index + 3]) for index in range(0, 12, 3)]
            cv2.imwrite(str(output / f"camera-grid-{algorithm}-{page}.png"), np.vstack(rows))


def run_native(executable: Path, dataset: Path, config: Path, control: Path,
               pass_name: str, frames: int, output: Path,
               resample_output: bool) -> dict:
    output.mkdir(parents=True, exist_ok=False)
    command = [str(executable), "--dataset-root", str(dataset),
               "--config", str(config), "--stereo-control", str(control),
               "--pass", pass_name, "--max-frames", str(frames),
               "--warmup", "1", "--run", "--output-dir", str(output)]
    if resample_output:
        command.append("--resample-output")
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    return json.loads((output / "summary.json").read_text(encoding="utf-8"))


def average(row: dict, key: str, prefix: str = "screen") -> float:
    return 0.5 * (row[f"{prefix}_clean_{key}"] + row[f"{prefix}_final_{key}"])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--camera-left", type=Path, required=True)
    parser.add_argument("--camera-right", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--screen-frames", type=int, default=32)
    parser.add_argument("--full-finalists", type=int, default=4)
    parser.add_argument("--scale", type=int, choices=(1, 2), default=1,
                        help="1 for native 728x544; 2 for 364x272 then upscale")
    parser.add_argument("--right-output-correction", type=float, nargs=6,
                        metavar=("H00", "H01", "H02", "H10", "H11", "H12"),
                        help="forward affine correction for the rectified right image")
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise ValueError("output directory must be new or empty")
    args.output.mkdir(parents=True, exist_ok=True)
    controls = args.output / "controls"
    controls.mkdir()
    cv2.setNumThreads(4)
    left = cv2.imread(str(args.camera_left), cv2.IMREAD_GRAYSCALE)
    right = cv2.imread(str(args.camera_right), cv2.IMREAD_GRAYSCALE)
    if left is None or right is None or left.shape != (544, 728) or right.shape != left.shape:
        raise ValueError("camera pair must be two 728x544 grayscale images")
    if args.right_output_correction:
        correction = np.asarray(args.right_output_correction, np.float64).reshape(2, 3)
        right = cv2.warpAffine(right, correction, (728, 544),
                               flags=cv2.INTER_LINEAR,
                               borderMode=cv2.BORDER_CONSTANT)

    profiles = profile_matrix(args.scale)
    rows = []
    camera_images: dict[str, np.ndarray] = {}
    for index, profile in enumerate(profiles, 1):
        print(f"[{index}/{len(profiles)}] {profile.name}", flush=True)
        control = controls / f"{profile.name}.json"
        control.write_text(json.dumps(profile.control(), separators=(",", ":")) + "\n",
                           encoding="utf-8")
        row = {"name": profile.name, **profile.control()}
        camera, camera_image = camera_score(profile, left, right, args.scale)
        row.update(camera)
        camera_images[profile.name] = labelled(camera_image, profile, camera)
        for pass_name in ("clean", "final"):
            summary = run_native(args.benchmark, args.dataset, args.config, control,
                                 pass_name, args.screen_frames,
                                 args.output / "native-screen" / profile.name / pass_name,
                                 args.scale == 2)
            for key, value in summary.items():
                if isinstance(value, (int, float)):
                    row[f"screen_{pass_name}_{key}"] = value
        row["screen_bad2_mean"] = average(row, "bad_2_including_invalid")
        row["screen_bad3_mean"] = average(row, "bad_3_including_invalid")
        row["screen_coverage_mean"] = average(row, "coverage")
        row["screen_mae_mean"] = average(row, "mae_valid_only_px")
        row["screen_latency_mean_ms"] = average(row, "matcher_ms_p50")
        rows.append(row)

    ranked = sorted(rows, key=lambda r: (r["screen_bad2_mean"],
                                         r["screen_mae_mean"],
                                         -r["screen_coverage_mean"]))
    # Search ranges change both the valid ROI and eligible truth interval.
    # Select finalists only at the product-equivalent 96-pixel interval;
    # report other ranges as geometry/performance tradeoffs instead.
    comparison_range = 96 if args.scale == 1 else 48
    comparable = [row for row in ranked
                  if int(row["search_range"]) == comparison_range]
    finalists = []
    for algorithm in ("sgbm", "bm"):
        finalists.append(next(row for row in comparable
                              if row["algorithm"] == algorithm))
    for row in comparable:
        if row not in finalists:
            finalists.append(row)
        if len(finalists) >= args.full_finalists:
            break
    for finalist in finalists:
        control = controls / f"{finalist['name']}.json"
        for pass_name in ("clean", "final"):
            print(f"[full] {finalist['name']} {pass_name}", flush=True)
            summary = run_native(args.benchmark, args.dataset, args.config, control,
                                 pass_name, 999999,
                                 args.output / "native-full" / finalist["name"] / pass_name,
                                 args.scale == 2)
            for key, value in summary.items():
                if isinstance(value, (int, float)):
                    finalist[f"full_{pass_name}_{key}"] = value
        finalist["full_bad2_mean"] = average(finalist, "bad_2_including_invalid", "full")
        finalist["full_bad3_mean"] = average(finalist, "bad_3_including_invalid", "full")
        finalist["full_coverage_mean"] = average(finalist, "coverage", "full")
        finalist["full_mae_mean"] = average(finalist, "mae_valid_only_px", "full")
        finalist["full_latency_mean_ms"] = average(finalist, "matcher_ms_p50", "full")

    fields = sorted({key for row in rows for key in row})
    with (args.output / "results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fields)
        writer.writeheader()
        writer.writerows(rows)
    grids(profiles, camera_images, args.output)
    epipolar = epipolar_metrics(left, right)
    (args.output / "epipolar.json").write_text(json.dumps(epipolar, indent=2) + "\n",
                                                encoding="utf-8")
    recommendation = {
        "screen_frame_count_per_pass": args.screen_frames,
        "profile_count": len(profiles),
        "matching_scale": args.scale,
        "right_output_correction": args.right_output_correction,
        "finalists": [{key: value for key, value in row.items()
                        if key in ("name", "algorithm", "screen_bad2_mean",
                                   "screen_coverage_mean", "screen_mae_mean",
                                   "full_bad2_mean", "full_coverage_mean",
                                   "full_mae_mean", "full_latency_mean_ms")}
                       for row in finalists],
        "best_dataset_profile": finalists[0]["name"],
        "epipolar": epipolar,
    }
    (args.output / "recommendation.json").write_text(
        json.dumps(recommendation, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(recommendation, indent=2))


if __name__ == "__main__":
    main()
