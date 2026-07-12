#!/usr/bin/env python3
"""Split LAS2 convex upsampling from a full Hailo-oriented ONNX graph.

The Hailo graph retains feature extraction, correlation, cost aggregation,
quarter-resolution disparity regression, refinement, and the 9-way softmax.
Only the fixed 9-tap full-resolution weighted sum moves to the host.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnx import shape_inference, utils


def tensor_shapes(model: onnx.ModelProto) -> dict[str, list[int]]:
    inferred = shape_inference.infer_shapes(model)
    return {
        value.name: [dimension.dim_value for dimension in value.type.tensor_type.shape.dim]
        for value in [*inferred.graph.value_info, *inferred.graph.output]
    }


def find_output(
    model: onnx.ModelProto,
    shapes: dict[str, list[int]],
    shape: list[int],
    op: str,
    name_fragment: str = "",
) -> str:
    matches = [
        output
        for node in model.graph.node
        if node.op_type == op and name_fragment in node.name
        for output in node.output
        if shapes.get(output) == shape
    ]
    if len(matches) != 1:
        raise RuntimeError(f"expected one {op} output with shape {shape}, got {matches}")
    return matches[0]


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
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()

    model = onnx.load(args.input)
    shapes = tensor_shapes(model)
    weight_shapes = [
        shapes[output]
        for node in model.graph.node
        if node.op_type == "Softmax"
        for output in node.output
        if len(shapes.get(output, [])) == 4 and shapes[output][1] == 9
    ]
    if len(weight_shapes) != 1:
        raise RuntimeError(f"expected one 9-channel softmax, got {weight_shapes}")
    weights_shape = weight_shapes[0]
    coarse_shape = [1, 1, weights_shape[2] // 4, weights_shape[3] // 4]
    coarse = find_output(
        model, shapes, coarse_shape, "Conv", "disparity_regression"
    )
    weights = find_output(model, shapes, weights_shape, "Softmax")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    utils.extract_model(
        str(args.input), str(args.output), ["left", "right"], [coarse, weights]
    )
    onnx.checker.check_model(onnx.load(args.output))

    random = np.random.default_rng(args.seed)
    input_shape = (1, 3, weights_shape[2], weights_shape[3])
    inputs = {
        name: random.uniform(0.0, 255.0, input_shape).astype(np.float32)
        for name in ("left", "right")
    }
    full_session = ort.InferenceSession(str(args.input), providers=["CPUExecutionProvider"])
    split_session = ort.InferenceSession(str(args.output), providers=["CPUExecutionProvider"])
    expected = full_session.run(None, inputs)[0]
    split_outputs = dict(
        zip(
            [output.name for output in split_session.get_outputs()],
            split_session.run(None, inputs),
        )
    )
    candidate = convex_upsample(split_outputs[coarse], split_outputs[weights])
    difference = np.abs(expected - candidate)
    parity = {
        "mae": float(difference.mean()),
        "max_abs": float(difference.max()),
        "p99_abs": float(np.quantile(difference, 0.99)),
    }
    if parity["mae"] > 1e-5 or parity["max_abs"] > 1e-3:
        raise RuntimeError(f"split postprocess parity failed: {parity}")

    manifest = {
        "input": str(args.input),
        "outputs": {
            "disparity_quarter": {"name": coarse, "shape": coarse_shape},
            "upsample_weights": {"name": weights, "shape": weights_shape},
        },
        "host_postprocess": "zero-border 9-tap convex upsample, stride 4",
        "parity": parity,
    }
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
