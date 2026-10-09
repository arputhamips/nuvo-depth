#!/usr/bin/env python3
"""Three-capture visual and stability gate for SGBM smoothing candidates."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path

import cv2
import numpy as np

from extract_nuvo_frame import parse_packet
from stereo_validation_matrix import (Profile, camera_score,
                                      invalid_aware_upscale, valid_disparity)


def pairs(root: Path, count: int) -> list[tuple[int, Path | np.ndarray, Path | np.ndarray]]:
    found = []
    for left in sorted(root.glob("rectified_left_*.png"),
                       key=lambda p: int(re.search(r"(\d+)$", p.stem).group(1))):
        sequence = int(re.search(r"(\d+)$", left.stem).group(1))
        right = root / f"rectified_right_{sequence}.png"
        if right.is_file():
            found.append((sequence, left, right))
    if len(found) < count:
        found = []
        packets = sorted(root.glob("post_depth_*.nuv"),
                         key=lambda p: int(re.search(r"(\d+)$", p.stem).group(1)))
        for packet in packets:
            sequence = int(re.search(r"(\d+)$", packet.stem).group(1))
            metadata, payload = parse_packet(packet)
            images = {}
            for product in metadata.get("products", []):
                if product["kind"] not in {"left_rectified", "right_rectified"}:
                    continue
                start = int(product["offset"])
                stop = start + int(product["length"])
                images[product["kind"]] = np.frombuffer(
                    payload[start:stop], np.uint8
                ).reshape(int(product["height"]), int(product["width"])).copy()
            if len(images) == 2:
                found.append((sequence, images["left_rectified"],
                              images["right_rectified"]))
    if len(found) < count:
        raise ValueError(f"need {count} paired rectified captures; found {len(found)}")
    indices = np.linspace(0, len(found) - 1, count).round().astype(int)
    return [found[index] for index in indices]


def profile_from(path: Path) -> Profile:
    value = json.loads(path.read_text(encoding="utf-8"))
    value.pop("schema_version")
    return Profile(name=path.stem, **value)


def smoothness(left: np.ndarray, disparity: np.ndarray, valid: np.ndarray) -> dict:
    filled = np.nan_to_num(disparity, nan=0.0).astype(np.float32)
    local = cv2.medianBlur(filled, 5)
    gradient = cv2.magnitude(cv2.Sobel(left, cv2.CV_32F, 1, 0),
                             cv2.Sobel(left, cv2.CV_32F, 0, 1))
    interior = valid & (cv2.erode(valid.astype(np.uint8), np.ones((5, 5), np.uint8)) != 0)
    flat = interior & (gradient <= np.percentile(gradient[interior], 60)) if np.any(interior) else interior
    roughness = np.abs(filled - local)[flat]
    disparity_dx = np.abs(cv2.Sobel(filled, cv2.CV_32F, 1, 0, ksize=3))
    disparity_dy = np.abs(cv2.Sobel(filled, cv2.CV_32F, 0, 1, ksize=3))
    # Horizontal streaks have a strong vertical transition but remain nearly
    # constant along the row. Restrict the score to matcher-valid, low-texture
    # interior pixels so true object edges do not dominate it.
    streak = flat & (disparity_dy >= 2.0) & (disparity_dx <= 0.5)
    values = disparity[valid]
    return {
        "flat_roughness_p50_px": float(np.percentile(roughness, 50)) if roughness.size else float("nan"),
        "flat_roughness_p95_px": float(np.percentile(roughness, 95)) if roughness.size else float("nan"),
        "subpixel_levels": int(np.unique(np.round(values * 16)).size) if values.size else 0,
        "horizontal_streak_fraction": float(np.sum(streak) / max(1, np.sum(flat))),
        "vertical_disparity_gradient_p95_px": (
            float(np.percentile(disparity_dy[flat] / 8.0, 95))
            if np.any(flat) else float("nan")),
    }


def grayscale_products(disparity: np.ndarray, valid: np.ndarray,
                       maximum: float, focal: float = 493.542046188,
                       baseline: float = 0.1) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    disparity_gray = np.zeros(disparity.shape, np.uint8)
    disparity_gray[valid] = np.clip(disparity[valid] * 255.0 / maximum,
                                    0, 255).astype(np.uint8)
    depth = np.full(disparity.shape, np.nan, np.float32)
    positive = valid & (disparity > 0)
    depth[positive] = focal * baseline / disparity[positive]
    depth_gray = np.zeros(disparity.shape, np.uint8)
    depth_gray[positive] = np.clip((12.0 - depth[positive]) * 255.0 / 11.5,
                                   0, 255).astype(np.uint8)
    return disparity_gray, depth_gray, depth


def title(image: np.ndarray, text: str) -> np.ndarray:
    background = (18, 18, 18) if image.ndim == 3 else 18
    foreground = (230, 230, 230) if image.ndim == 3 else 230
    panel = cv2.copyMakeBorder(image, 34, 0, 0, 0,
                               cv2.BORDER_CONSTANT, value=background)
    cv2.putText(panel, text, (8, 23), cv2.FONT_HERSHEY_SIMPLEX, 0.52,
                foreground, 1, cv2.LINE_AA)
    return panel


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pairs", type=Path, required=True)
    parser.add_argument("--controls", type=Path, required=True)
    parser.add_argument("--control", type=Path, action="append",
                        help="specific control file; repeat to override directory glob")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--captures", type=int, default=3)
    parser.add_argument("--scale", type=int, choices=(1, 2), default=1,
                        help="2 matches at 364x272 and invalid-aware upscales")
    parser.add_argument("--right-output-correction", type=float, nargs=6,
                        metavar=("H00", "H01", "H02", "H10", "H11", "H12"),
                        help="forward affine correction for every rectified right image")
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise ValueError("output directory must be new or empty")
    args.output.mkdir(parents=True, exist_ok=True)
    selected_pairs = pairs(args.pairs, args.captures)
    control_paths = args.control or sorted(args.controls.glob("*.json"))
    profiles = [profile_from(path) for path in control_paths]
    cv2.setNumThreads(4)
    rows = []
    visual_rows = []
    color_visual_rows = []
    for profile in profiles:
        print(profile.name, flush=True)
        disparities = []
        masks = []
        tiles = []
        color_tiles = []
        for sequence, left_source, right_source in selected_pairs:
            left = (left_source if isinstance(left_source, np.ndarray)
                    else cv2.imread(str(left_source), cv2.IMREAD_GRAYSCALE))
            right = (right_source if isinstance(right_source, np.ndarray)
                     else cv2.imread(str(right_source), cv2.IMREAD_GRAYSCALE))
            if args.right_output_correction:
                correction = np.asarray(args.right_output_correction,
                                        np.float64).reshape(2, 3)
                right = cv2.warpAffine(right, correction, (left.shape[1], left.shape[0]),
                                       flags=cv2.INTER_LINEAR,
                                       borderMode=cv2.BORDER_CONSTANT)
            match_left = left if args.scale == 1 else cv2.resize(
                left, (364, 272), interpolation=cv2.INTER_AREA)
            match_right = right if args.scale == 1 else cv2.resize(
                right, (364, 272), interpolation=cv2.INTER_AREA)
            disparity, valid, roi, elapsed = valid_disparity(
                profile, match_left, match_right)
            if args.scale == 2:
                disparity, valid, roi = invalid_aware_upscale(disparity, valid, roi)
            metrics, _ = camera_score(profile, left, right, args.scale)
            metrics.update(smoothness(left, disparity, valid))
            disparity_gray, depth_gray, depth = grayscale_products(
                disparity, valid, profile.max_disparity * args.scale)
            disparity_color = cv2.applyColorMap(disparity_gray,
                                                 cv2.COLORMAP_TURBO)
            disparity_color[~valid] = 0
            row = {"profile": profile.name, "sequence": sequence,
                   "median_filter_size": profile.median_filter_size,
                   "block_size": profile.block_size,
                   "uniqueness_ratio": profile.uniqueness_ratio,
                   **metrics, "matcher_ms": elapsed,
                   "valid_depth_p50_m": float(np.nanmedian(depth))}
            rows.append(row)
            disparities.append(disparity)
            masks.append(valid)
            disparity_tile = cv2.resize(title(disparity_gray,
                                               f"{profile.name} / frame {sequence}"),
                                        (364, 289), interpolation=cv2.INTER_AREA)
            tiles.append(disparity_tile)
            color_tiles.append(cv2.resize(
                title(disparity_color,
                      f"{profile.name} / frame {sequence} / TURBO"),
                (364, 289), interpolation=cv2.INTER_AREA))
            montage = np.vstack((np.hstack((title(left, "LEFT RECTIFIED"),
                                             title(right, "RIGHT RECTIFIED"))),
                                 np.hstack((title(disparity_gray, "DISPARITY / GRAY"),
                                             title(depth_gray, "DEPTH 0.5-12 M / GRAY")))))
            cv2.imwrite(str(args.output / f"{profile.name}-frame-{sequence}.png"), montage)
        visual_rows.append(np.hstack(tiles))
        color_visual_rows.append(np.hstack(color_tiles))
        stack = np.stack(disparities)
        common = np.logical_and.reduce(masks)
        temporal = np.std(stack[:, common], axis=0) if np.any(common) else np.array([])
        for row in rows[-len(selected_pairs):]:
            row["three_capture_common_valid_fraction"] = float(np.mean(common))
            row["three_capture_std_p50_px"] = float(np.percentile(temporal, 50)) if temporal.size else float("nan")
            row["three_capture_std_p95_px"] = float(np.percentile(temporal, 95)) if temporal.size else float("nan")
    with (args.output / "results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    cv2.imwrite(str(args.output / "three-capture-grayscale-grid.png"),
                np.vstack(visual_rows))
    cv2.imwrite(str(args.output / "three-capture-color-grid.png"),
                np.vstack(color_visual_rows))
    summary = []
    for profile in profiles:
        selected = [row for row in rows if row["profile"] == profile.name]
        summary.append({
            "profile": profile.name,
            "valid_fraction_mean": float(np.mean([r["camera_valid_fraction"] for r in selected])),
            "photometric_mae_mean": float(np.mean([r["camera_photometric_mae"] for r in selected])),
            "roughness_p95_mean_px": float(np.mean([r["flat_roughness_p95_px"] for r in selected])),
            "horizontal_streak_fraction_mean": float(np.mean(
                [r["horizontal_streak_fraction"] for r in selected])),
            "vertical_disparity_gradient_p95_mean_px": float(np.mean(
                [r["vertical_disparity_gradient_p95_px"] for r in selected])),
            "small_component_pixels_mean": float(np.mean([r["camera_small_component_pixels"] for r in selected])),
            "three_capture_common_valid_fraction": selected[0]["three_capture_common_valid_fraction"],
            "three_capture_std_p95_px": selected[0]["three_capture_std_p95_px"],
            "matcher_ms_mean": float(np.mean([r["matcher_ms"] for r in selected])),
        })
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                               encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
