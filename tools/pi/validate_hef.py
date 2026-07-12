#!/usr/bin/env python3
"""Run a two-input LAS2 HEF on Hailo-8L and save disparity diagnostics."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import cv2
import numpy as np
from hailo_platform import FormatType, HailoSchedulingAlgorithm, VDevice


WIDTH = 728
MODEL_WIDTH = 736
HEIGHT = 544
FOCAL_LENGTH_PX = 493.542046188
BASELINE_M = 0.161458780


def load_rgb(path: Path, model_width: int, model_height: int) -> np.ndarray:
    image = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if image is None:
        raise RuntimeError(f"failed to read {path}")
    image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    if image.shape[:2] != (HEIGHT, WIDTH):
        raise RuntimeError(f"expected {WIDTH}x{HEIGHT}, got {image.shape[1]}x{image.shape[0]}")
    if (model_width, model_height) == (MODEL_WIDTH, HEIGHT):
        return cv2.copyMakeBorder(image, 0, 0, 4, 4, cv2.BORDER_REPLICATE)
    return cv2.resize(image, (model_width, model_height), interpolation=cv2.INTER_AREA)


def shift_zero(tensor: np.ndarray, dy: int, dx: int, stride: int = 4) -> np.ndarray:
    height, width = tensor.shape[-2:]
    pad_left = max(-dx * stride, 0)
    pad_right = max(dx * stride, 0)
    pad_top = max(-dy * stride, 0)
    pad_bottom = max(dy * stride, 0)
    padded = np.pad(
        tensor,
        ((0, 0), (0, 0), (pad_top, pad_bottom), (pad_left, pad_right)),
    )
    start_x = max(dx * stride, 0)
    start_y = max(dy * stride, 0)
    return padded[:, :, start_y : start_y + height, start_x : start_x + width]


def convex_upsample(disparity: np.ndarray, weights: np.ndarray) -> np.ndarray:
    high = np.repeat(np.repeat(disparity * 4.0, 4, axis=2), 4, axis=3)
    terms = []
    index = 0
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            terms.append(shift_zero(high, dy, dx) * weights[:, index : index + 1])
            index += 1
    while len(terms) > 1:
        terms = [
            terms[index]
            if index + 1 == len(terms)
            else terms[index] + terms[index + 1]
            for index in range(0, len(terms), 2)
        ]
    return terms[0]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hef", type=Path, required=True)
    parser.add_argument("--left", type=Path, required=True)
    parser.add_argument("--right", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--reference-disparity", type=Path)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=20)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    params = VDevice.create_params()
    params.scheduling_algorithm = HailoSchedulingAlgorithm.ROUND_ROBIN
    device = VDevice(params)
    try:
        infer_model = device.create_infer_model(str(args.hef))
        infer_model.set_batch_size(1)
        if len(infer_model.inputs) != 2 or len(infer_model.outputs) not in (1, 2):
            raise RuntimeError(
                f"expected two inputs and one or two outputs, got "
                f"{len(infer_model.inputs)} and {len(infer_model.outputs)}"
            )
        for stream in infer_model.inputs:
            stream.set_format_type(FormatType.UINT8)
        for stream in infer_model.outputs:
            stream.set_format_type(FormatType.FLOAT32)

        input_names = list(infer_model.input_names)
        input_shape = infer_model.input(input_names[0]).shape
        model_height, model_width = input_shape[:2]
        inputs = [
            load_rgb(args.left, model_width, model_height),
            load_rgb(args.right, model_width, model_height),
        ]
        output_names = list(infer_model.output_names)
        outputs = {
            name: np.empty(infer_model.output(name).shape, dtype=np.float32)
            for name in output_names
        }
        configured = infer_model.configure()
        bindings = configured.create_bindings(
            input_buffers=dict(zip(input_names, inputs, strict=True)),
            output_buffers=outputs,
        )

        timings_ms = []
        for index in range(args.warmup + args.iterations):
            configured.wait_for_async_ready(timeout_ms=10000)
            started = time.perf_counter_ns()
            configured.run([bindings], 10000)
            elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000.0
            if index >= args.warmup:
                timings_ms.append(elapsed_ms)
    finally:
        device.release()

    postprocess_started = time.perf_counter_ns()
    if len(outputs) == 1:
        disparity = np.squeeze(next(iter(outputs.values())))
    else:
        coarse_hwc = next(value for value in outputs.values() if value.shape[-1] == 1)
        weights_hwc = next(value for value in outputs.values() if value.shape[-1] == 9)
        coarse = coarse_hwc.transpose(2, 0, 1)[None]
        weights = weights_hwc.transpose(2, 0, 1)[None]
        disparity = convex_upsample(coarse, weights)[0, 0]
    postprocess_ms = (time.perf_counter_ns() - postprocess_started) / 1_000_000.0
    if disparity.shape == (HEIGHT, MODEL_WIDTH):
        disparity = disparity[:, 4:-4].copy()
    elif disparity.shape != (HEIGHT, WIDTH):
        source_width = disparity.shape[1]
        disparity = cv2.resize(
            disparity, (WIDTH, HEIGHT), interpolation=cv2.INTER_LINEAR
        )
        disparity *= WIDTH / source_width
    valid = np.isfinite(disparity) & (disparity > 0.0) & (disparity < 192.0)
    depth = np.full_like(disparity, np.nan)
    depth[valid] = FOCAL_LENGTH_PX * BASELINE_M / disparity[valid]

    np.save(args.output_dir / "disparity.npy", disparity)
    np.save(args.output_dir / "depth_m.npy", depth)
    visualization = np.zeros_like(disparity, dtype=np.uint8)
    if np.any(valid):
        lo, hi = np.percentile(disparity[valid], [2, 98])
        visualization[valid] = np.clip(
            (disparity[valid] - lo) * 255.0 / max(hi - lo, 1e-6), 0, 255
        ).astype(np.uint8)
    color = cv2.applyColorMap(visualization, cv2.COLORMAP_TURBO)
    color[~valid] = 0
    cv2.imwrite(str(args.output_dir / "disparity.png"), color)

    reference_comparison = None
    if args.reference_disparity is not None:
        reference = np.load(args.reference_disparity)
        if reference.shape != disparity.shape:
            raise RuntimeError(
                f"reference shape {reference.shape} does not match {disparity.shape}"
            )
        comparable = np.isfinite(reference) & np.isfinite(disparity)
        absolute_error = np.abs(reference - disparity)
        values = absolute_error[comparable]
        if values.size == 0:
            raise RuntimeError("reference and Hailo outputs have no comparable pixels")
        reference_comparison = {
            "comparable_fraction": float(comparable.mean()),
            "mae_px": float(values.mean()),
            "max_abs_px": float(values.max()),
            "p95_abs_px": float(np.percentile(values, 95)),
            "p99_abs_px": float(np.percentile(values, 99)),
        }
        reference_valid = np.isfinite(reference) & (reference > 0.0) & (reference < 192.0)
        reference_visualization = np.zeros_like(reference, dtype=np.uint8)
        if np.any(reference_valid):
            reference_low, reference_high = np.percentile(
                reference[reference_valid], [2, 98]
            )
            reference_visualization[reference_valid] = np.clip(
                (reference[reference_valid] - reference_low)
                * 255.0
                / max(reference_high - reference_low, 1e-6),
                0,
                255,
            ).astype(np.uint8)
        reference_color = cv2.applyColorMap(
            reference_visualization, cv2.COLORMAP_TURBO
        )
        reference_color[~reference_valid] = 0
        cv2.imwrite(str(args.output_dir / "reference_disparity.png"), reference_color)
        difference_visualization = np.zeros_like(disparity, dtype=np.uint8)
        if values.size:
            upper = max(float(np.percentile(values, 99)), 1e-6)
            difference_visualization[comparable] = np.clip(
                absolute_error[comparable] * 255.0 / upper, 0, 255
            ).astype(np.uint8)
        difference_color = cv2.applyColorMap(
            difference_visualization, cv2.COLORMAP_MAGMA
        )
        difference_color[~comparable] = 0
        cv2.imwrite(str(args.output_dir / "difference.png"), difference_color)

    timing_array = np.asarray(timings_ms)
    summary = {
        "input_names": input_names,
        "output_names": output_names,
        "output_shapes": {name: list(value.shape) for name, value in outputs.items()},
        "iterations": args.iterations,
        "latency_ms_p50": float(np.percentile(timing_array, 50)),
        "latency_ms_p95": float(np.percentile(timing_array, 95)),
        "fps_from_mean_latency": float(1000.0 / timing_array.mean()),
        "host_postprocess_ms": postprocess_ms,
        "valid_fraction": float(valid.mean()),
        "disparity_min": float(np.nanmin(disparity)),
        "disparity_max": float(np.nanmax(disparity)),
        "median_depth_m": float(np.nanmedian(depth)),
        "reference_comparison": reference_comparison,
    }
    (args.output_dir / "validation.json").write_text(
        json.dumps(summary, indent=2) + "\n"
    )
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
