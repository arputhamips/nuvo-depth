#!/usr/bin/env python3
"""Stage and score production SGBM parameter sweeps on recorded stereo pairs.

The sweep intentionally changes one parameter at a time around the two geometry
finalists.  This keeps the results interpretable while covering every OpenCV
SGBM setting that materially affects confidence, smoothing, or runtime.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import time
from dataclasses import asdict, dataclass, replace
from pathlib import Path

import cv2
import numpy as np


@dataclass(frozen=True)
class Profile:
    name: str
    block_size: int = 5
    uniqueness_ratio: int = 1
    speckle_window_size: int = 30
    speckle_range: int = 2
    disp12_max_diff: int = 1
    pre_filter_cap: int = 31
    mode: int = cv2.STEREO_SGBM_MODE_SGBM_3WAY
    p1_scale: float = 1.0
    p2_p1_ratio: float = 4.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pairs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    return parser.parse_args()


def profiles() -> list[Profile]:
    result: list[Profile] = []
    for block in (5, 7):
        base = Profile(name=f"b{block}_base", block_size=block)
        result.append(base)
        groups = (
            ("u", "uniqueness_ratio", (0, 3, 5, 8, 10, 15)),
            ("sw", "speckle_window_size", (0, 10, 20, 50, 100)),
            ("sr", "speckle_range", (1, 4)),
            ("lr", "disp12_max_diff", (-1, 2, 4)),
            ("pf", "pre_filter_cap", (15, 63)),
            (
                "mode",
                "mode",
                (
                    cv2.STEREO_SGBM_MODE_SGBM,
                    cv2.STEREO_SGBM_MODE_HH,
                    cv2.STEREO_SGBM_MODE_HH4,
                ),
            ),
            ("p1", "p1_scale", (0.5, 0.75, 1.5, 2.0)),
            ("p2r", "p2_p1_ratio", (2.0, 6.0, 8.0)),
        )
        for prefix, field, values in groups:
            for value in values:
                label = str(value).replace("-", "m").replace(".", "p")
                result.append(
                    replace(base, name=f"b{block}_{prefix}{label}", **{field: value})
                )
    return result


def load_pairs(root: Path) -> list[tuple[np.ndarray, np.ndarray]]:
    result = []
    for directory in sorted(root.glob("pair*")):
        if not directory.is_dir():
            continue
        left = cv2.imread(str(directory / "left_rect.png"), cv2.IMREAD_GRAYSCALE)
        right = cv2.imread(str(directory / "right_rect.png"), cv2.IMREAD_GRAYSCALE)
        if left is not None and right is not None:
            if left.shape != (544, 728) or right.shape != left.shape:
                raise ValueError(f"unexpected image geometry in {directory}")
            result.append((left, right))
    if len(result) < 2:
        raise ValueError("at least two complete pair directories are required")
    return result


def make_matcher(profile: Profile, disparities: int = 64) -> cv2.StereoSGBM:
    area = profile.block_size * profile.block_size
    p1 = max(1, round(8 * area * profile.p1_scale))
    p2 = max(p1 + 1, round(p1 * profile.p2_p1_ratio))
    return cv2.StereoSGBM_create(
        minDisparity=0,
        numDisparities=disparities,
        blockSize=profile.block_size,
        P1=p1,
        P2=p2,
        disp12MaxDiff=profile.disp12_max_diff,
        preFilterCap=profile.pre_filter_cap,
        uniquenessRatio=profile.uniqueness_ratio,
        speckleWindowSize=profile.speckle_window_size,
        speckleRange=profile.speckle_range,
        mode=profile.mode,
    )


def compute_working(
    matcher: cv2.StereoSGBM, pair: tuple[np.ndarray, np.ndarray]
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    left = cv2.resize(pair[0], (364, 272), interpolation=cv2.INTER_AREA)
    right = cv2.resize(pair[1], (364, 272), interpolation=cv2.INTER_AREA)
    fixed = matcher.compute(left, right)
    disparity = fixed.astype(np.float32) / 16.0
    valid = fixed > 0
    return left, right, disparity, valid


def native_references(
    pairs: list[tuple[np.ndarray, np.ndarray]],
) -> list[np.ndarray]:
    """Generate a higher-detail full-resolution reference for relative scoring."""
    matcher = make_matcher(Profile(name="native_reference"), disparities=128)
    outputs = []
    for left, right in pairs:
        fixed = matcher.compute(left, right)
        disparity = fixed.astype(np.float32) / 16.0
        valid = (fixed > 0) & (disparity < 128.0)
        disparity[~valid] = np.nan
        outputs.append(disparity)
    return outputs


def upscale(disparity: np.ndarray, valid: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    full = cv2.resize(disparity, (728, 544), interpolation=cv2.INTER_LINEAR) * 2.0
    full_valid = cv2.resize(valid.astype(np.uint8), (728, 544), interpolation=cv2.INTER_NEAREST)
    full_valid = (full_valid != 0) & (full < 128.0)
    full[~full_valid] = np.nan
    return full, full_valid


def photometric_metrics(
    left: np.ndarray, right: np.ndarray, disparity: np.ndarray, valid: np.ndarray
) -> tuple[float, float]:
    yy, xx = np.indices(left.shape, dtype=np.float32)
    warped = cv2.remap(
        right,
        xx - disparity,
        yy,
        interpolation=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_CONSTANT,
    )
    usable = valid & ((xx - disparity) >= 0.0)
    residual = np.abs(left.astype(np.float32) - warped.astype(np.float32))[usable]
    if residual.size == 0:
        return float("nan"), float("nan")
    return float(np.mean(residual)), float(np.percentile(residual, 95))


def fragmentation(valid: np.ndarray) -> tuple[int, int]:
    count, _, stats, _ = cv2.connectedComponentsWithStats(valid.astype(np.uint8), 8)
    areas = stats[1:, cv2.CC_STAT_AREA] if count > 1 else np.empty(0, dtype=np.int32)
    small = areas[areas < 100]
    return int(small.size), int(np.sum(small))


def temporal_metrics(outputs: list[np.ndarray]) -> tuple[float, float, float]:
    # pair00 predates the controlled five-pair capture.  Use the controlled set.
    stack = np.stack(outputs[1:] if len(outputs) > 2 else outputs)
    common = np.all(np.isfinite(stack), axis=0)
    if not np.any(common):
        return 0.0, float("nan"), float("nan")
    standard_deviation = np.std(stack[:, common], axis=0)
    return (
        float(np.mean(common)),
        float(np.percentile(standard_deviation, 50)),
        float(np.percentile(standard_deviation, 95)),
    )


def native_agreement(
    outputs: list[np.ndarray], references: list[np.ndarray]
) -> tuple[float, float, float, float, float]:
    common_fractions = []
    reference_recalls = []
    profile_only_fractions = []
    median_errors = []
    bad_two = []
    for output, reference in zip(outputs, references):
        output_valid = np.isfinite(output)
        reference_valid = np.isfinite(reference)
        common = output_valid & reference_valid
        common_fractions.append(float(np.mean(common)))
        reference_recalls.append(float(np.sum(common) / max(1, np.sum(reference_valid))))
        profile_only_fractions.append(float(np.mean(output_valid & ~reference_valid)))
        error = np.abs(output[common] - reference[common])
        median_errors.append(float(np.median(error)) if error.size else float("nan"))
        bad_two.append(float(np.mean(error > 2.0)) if error.size else float("nan"))
    return (
        float(np.mean(common_fractions)),
        float(np.mean(reference_recalls)),
        float(np.mean(profile_only_fractions)),
        float(np.mean(median_errors)),
        float(np.mean(bad_two)),
    )


def score(
    profile: Profile,
    pairs: list[tuple[np.ndarray, np.ndarray]],
    references: list[np.ndarray],
    repeats: int,
) -> dict:
    matcher = make_matcher(profile)
    # Warm the selected implementation and its allocations.
    compute_working(matcher, pairs[0])
    wall_samples = []
    cpu_samples = []
    for _ in range(repeats):
        for pair in pairs:
            wall_start = time.perf_counter()
            cpu_start = time.process_time()
            compute_working(matcher, pair)
            cpu_samples.append((time.process_time() - cpu_start) * 1000.0)
            wall_samples.append((time.perf_counter() - wall_start) * 1000.0)

    outputs = []
    valid_fractions = []
    searchable_fractions = []
    photo_mae = []
    photo_p95 = []
    first_valid = None
    first_color = None
    for pair in pairs:
        left, right, disparity, valid = compute_working(matcher, pair)
        full, full_valid = upscale(disparity, valid)
        outputs.append(full)
        valid_fractions.append(float(np.mean(full_valid)))
        searchable_fractions.append(float(np.mean(full_valid[:, 128:])))
        mae, p95 = photometric_metrics(left, right, disparity, valid)
        photo_mae.append(mae)
        photo_p95.append(p95)
        if first_valid is None:
            first_valid = full_valid
            normalized = np.nan_to_num(full, nan=0.0)
            normalized = np.clip(normalized * (255.0 / 128.0), 0, 255).astype(np.uint8)
            first_color = cv2.applyColorMap(normalized, cv2.COLORMAP_TURBO)
            first_color[~full_valid] = 0

    component_count, component_pixels = fragmentation(first_valid)
    temporal_common, temporal_p50, temporal_p95 = temporal_metrics(outputs)
    native_common, native_recall, profile_only, native_median, native_bad_two = (
        native_agreement(outputs, references)
    )
    return {
        **asdict(profile),
        "mode_name": {
            cv2.STEREO_SGBM_MODE_SGBM: "sgbm",
            cv2.STEREO_SGBM_MODE_HH: "hh",
            cv2.STEREO_SGBM_MODE_SGBM_3WAY: "sgbm_3way",
            cv2.STEREO_SGBM_MODE_HH4: "hh4",
        }.get(profile.mode, str(profile.mode)),
        "wall_ms_p50": float(np.percentile(wall_samples, 50)),
        "wall_ms_p95": float(np.percentile(wall_samples, 95)),
        "cpu_ms_p50": float(np.percentile(cpu_samples, 50)),
        "valid_fraction": float(np.mean(valid_fractions)),
        "searchable_valid_fraction": float(np.mean(searchable_fractions)),
        "photo_mae": float(np.mean(photo_mae)),
        "photo_p95": float(np.mean(photo_p95)),
        "small_components": component_count,
        "small_component_pixels": component_pixels,
        "temporal_common_fraction": temporal_common,
        "temporal_std_p50": temporal_p50,
        "temporal_std_p95": temporal_p95,
        "native_common_fraction": native_common,
        "native_recall": native_recall,
        "profile_only_fraction": profile_only,
        "native_error_median": native_median,
        "native_bad_two_fraction": native_bad_two,
        "_first_color": first_color,
    }


def main() -> None:
    args = parse_args()
    if args.repeats < 1:
        raise ValueError("--repeats must be positive")
    cv2.setNumThreads(1)
    pairs = load_pairs(args.pairs)
    references = native_references(pairs)
    args.output.mkdir(parents=True, exist_ok=True)
    rows = []
    sweep_profiles = profiles()
    for index, profile in enumerate(sweep_profiles, start=1):
        print(f"[{index:02d}/{len(sweep_profiles)}] {profile.name}", flush=True)
        row = score(profile, pairs, references, args.repeats)
        cv2.imwrite(str(args.output / f"{profile.name}.png"), row.pop("_first_color"))
        rows.append(row)

    with (args.output / "results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (args.output / "run.json").write_text(
        json.dumps(
            {
                "pairs": str(args.pairs),
                "pair_count": len(pairs),
                "repeats": args.repeats,
                "opencv": cv2.__version__,
                "logical_cpus": os.cpu_count(),
                "profiles": len(rows),
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
