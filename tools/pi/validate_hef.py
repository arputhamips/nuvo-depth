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


def load_rgb(path: Path) -> np.ndarray:
    image = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if image is None:
        raise RuntimeError(f"failed to read {path}")
    image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    if image.shape[:2] != (HEIGHT, WIDTH):
        raise RuntimeError(f"expected {WIDTH}x{HEIGHT}, got {image.shape[1]}x{image.shape[0]}")
    return cv2.copyMakeBorder(image, 0, 0, 4, 4, cv2.BORDER_REPLICATE)


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
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=20)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    inputs = [load_rgb(args.left), load_rgb(args.right)]
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
        coarse_hwc = next(
            value for value in outputs.values() if value.shape == (136, 184, 1)
        )
        weights_hwc = next(
            value for value in outputs.values() if value.shape == (544, 736, 9)
        )
        coarse = coarse_hwc.transpose(2, 0, 1)[None]
        weights = weights_hwc.transpose(2, 0, 1)[None]
        disparity = convex_upsample(coarse, weights)[0, 0]
    postprocess_ms = (time.perf_counter_ns() - postprocess_started) / 1_000_000.0
    if disparity.shape != (HEIGHT, MODEL_WIDTH):
        raise RuntimeError(f"unexpected disparity shape {disparity.shape}")
    disparity = disparity[:, 4:-4].copy()
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
    }
    (args.output_dir / "validation.json").write_text(
        json.dumps(summary, indent=2) + "\n"
    )
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
