#!/usr/bin/env python3
"""Export the released LAS2-S checkpoint as a static Hailo-oriented ONNX graph.

The upstream implementation uses rank-5 correlation tensors and rank-6 unfold
tensors. Both are mathematically convenient but are unsuitable for Hailo DFC
3.x. This wrapper preserves the checkpoint weights while expressing correlation,
disparity regression, and convex upsampling using static rank-4 operations.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import torch
import torch.nn as nn
import torch.nn.functional as F
from onnx import helper, shape_inference
from onnxsim import simplify
from PIL import Image


INPUT_HEIGHT = 544
INPUT_WIDTH = 736
MAX_DISPARITY = 192
QUARTER_DISPARITIES = MAX_DISPARITY // 4
FEATURE_CHANNELS = 40
MAX_CONCAT_INPUTS = 4
CORRELATION_GROUP_SIZE = 4
REWRITE_MAE_LIMIT = 1e-4
REWRITE_MAX_ABS_LIMIT = 5e-3
ONNX_MAE_LIMIT = 2e-4
ONNX_MAX_ABS_LIMIT = 1e-2
END_TO_END_MAE_LIMIT = 3e-4
END_TO_END_MAX_ABS_LIMIT = 2e-2
IMAGENET_MEAN_255 = (123.675, 116.28, 103.53)
IMAGENET_STD_255 = (58.395, 57.12, 57.375)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_revision(path: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def frozen_conv(in_channels: int, weights: torch.Tensor) -> nn.Conv2d:
    layer = nn.Conv2d(in_channels, 1, kernel_size=1, bias=False)
    with torch.no_grad():
        layer.weight.copy_(weights.reshape(1, in_channels, 1, 1))
    layer.weight.requires_grad_(False)
    return layer


class FrozenInstanceNorm(nn.Module):
    """Per-channel affine approximation calibrated from InstanceNorm calls."""

    def __init__(self, scale: torch.Tensor, bias: torch.Tensor):
        super().__init__()
        channels = scale.numel()
        self.affine = nn.Conv2d(
            channels, channels, kernel_size=1, groups=channels, bias=True
        )
        with torch.no_grad():
            self.affine.weight.copy_(scale.reshape(channels, 1, 1, 1))
            self.affine.bias.copy_(bias)
        self.affine.weight.requires_grad_(False)
        self.affine.bias.requires_grad_(False)

    def forward(self, tensor: torch.Tensor) -> torch.Tensor:
        return self.affine(tensor)


def load_calibration_image(path: Path) -> torch.Tensor:
    pil_image = Image.open(path).convert("RGB")
    image = np.asarray(pil_image)
    if image.shape == (INPUT_HEIGHT, INPUT_WIDTH - 8, 3):
        image = np.pad(image, ((0, 0), (4, 4), (0, 0)), mode="edge")
    elif image.shape != (INPUT_HEIGHT, INPUT_WIDTH, 3):
        image = np.asarray(
            pil_image.resize((INPUT_WIDTH, INPUT_HEIGHT), Image.Resampling.BILINEAR)
        )
    if image.shape != (INPUT_HEIGHT, INPUT_WIDTH, 3):
        raise RuntimeError(f"expected 728x544 or 736x544 RGB image, got {image.shape}")
    return torch.from_numpy(image.copy()).permute(2, 0, 1).unsqueeze(0).float()


def replace_module(root: nn.Module, name: str, replacement: nn.Module) -> None:
    parts = name.split(".")
    parent = root
    for part in parts[:-1]:
        parent = parent[int(part)] if part.isdigit() else getattr(parent, part)
    last = parts[-1]
    if last.isdigit():
        parent[int(last)] = replacement
    else:
        setattr(parent, last, replacement)


def fold_affine_into_previous_conv(
    root: nn.Module, name: str, affine: FrozenInstanceNorm
) -> None:
    parts = name.split(".")
    parent = root
    for part in parts[:-1]:
        parent = parent[int(part)] if part.isdigit() else getattr(parent, part)
    index = int(parts[-1])
    convolution = parent[index - 1]
    if not isinstance(convolution, nn.Conv2d):
        raise RuntimeError(f"{name} does not follow a Conv2d")
    scale = affine.affine.weight[:, 0, 0, 0].detach()
    bias = affine.affine.bias.detach()
    with torch.no_grad():
        convolution.weight.mul_(scale.reshape(-1, 1, 1, 1))
        if convolution.bias is None:
            convolution.bias = nn.Parameter(bias.clone(), requires_grad=False)
        else:
            convolution.bias.mul_(scale).add_(bias)
            convolution.bias.requires_grad_(False)
    parent[index] = nn.Identity()


def frozen_affine(samples: list[torch.Tensor], epsilon: float) -> FrozenInstanceNorm:
    scales = []
    biases = []
    for sample in samples:
        mean = sample.mean(dim=(0, 2, 3))
        variance = sample.var(dim=(0, 2, 3), unbiased=False)
        scale = torch.rsqrt(variance + epsilon)
        scales.append(scale)
        biases.append(-mean * scale)
    return FrozenInstanceNorm(torch.stack(scales).mean(0), torch.stack(biases).mean(0))


def freeze_instance_norms(
    model: nn.Module, pairs: list[tuple[Path, Path]]
) -> tuple[nn.Module, nn.Module]:
    """Freeze dynamic norms; use separate left/right fnet calibration."""
    captured: dict[str, list[torch.Tensor]] = {}
    handles = []
    norms = {
        name: module
        for name, module in model.named_modules()
        if isinstance(module, nn.InstanceNorm2d)
    }
    for name, module in norms.items():
        captured[name] = []

        def hook(_: nn.Module, inputs: tuple[torch.Tensor, ...], key: str = name) -> None:
            captured[key].append(inputs[0].detach().cpu())

        handles.append(module.register_forward_pre_hook(hook))

    with torch.inference_mode():
        for left_path, right_path in pairs:
            model(
                load_calibration_image(left_path),
                load_calibration_image(right_path),
                max_disp=MAX_DISPARITY,
                test_mode=True,
            )
    for handle in handles:
        handle.remove()

    frozen = copy.deepcopy(model)
    right_fnet = copy.deepcopy(model.fnet)
    fnet_norm_name = next(name for name in norms if name.startswith("fnet."))
    fnet_relative_name = fnet_norm_name.removeprefix("fnet.")
    fnet_calls = captured[fnet_norm_name]
    fold_affine_into_previous_conv(
        frozen,
        fnet_norm_name,
        frozen_affine(fnet_calls[0::2], norms[fnet_norm_name].eps),
    )
    fold_affine_into_previous_conv(
        right_fnet,
        fnet_relative_name,
        frozen_affine(fnet_calls[1::2], norms[fnet_norm_name].eps),
    )
    for name, module in norms.items():
        if name != fnet_norm_name:
            fold_affine_into_previous_conv(
                frozen, name, frozen_affine(captured[name], module.eps)
            )
    return frozen, right_fnet


def add_tree(values: list[torch.Tensor]) -> torch.Tensor:
    current = values
    while len(current) > 1:
        current = [
            current[index]
            if index + 1 == len(current)
            else current[index] + current[index + 1]
            for index in range(0, len(current), 2)
        ]
    return current[0]


def shift_full_resolution(
    tensor: torch.Tensor, dy: int, dx: int, stride: int = 4
) -> torch.Tensor:
    """Return tensor[y + dy*stride, x + dx*stride] with zero borders."""
    height, width = tensor.shape[-2:]
    pad_left = max(-dx * stride, 0)
    pad_right = max(dx * stride, 0)
    pad_top = max(-dy * stride, 0)
    pad_bottom = max(dy * stride, 0)
    padded = F.pad(tensor, (pad_left, pad_right, pad_top, pad_bottom))
    start_x = max(dx * stride, 0)
    start_y = max(dy * stride, 0)
    return padded[:, :, start_y : start_y + height, start_x : start_x + width]


class HailoLas2S(nn.Module):
    """Weight-preserving LAS2-S graph expressed with DFC-friendly rank-4 ops."""

    def __init__(
        self,
        upstream_model: nn.Module,
        raw_rgb_input: bool = True,
        right_fnet: nn.Module | None = None,
    ):
        super().__init__()
        self.fnet = upstream_model.fnet
        self.fnet_right = right_fnet
        self.cost_agg = upstream_model.cost_agg
        self.refine_1 = upstream_model.refine_1
        self.stem_2 = upstream_model.stem_2
        self.refine_2 = upstream_model.refine_2
        self.refine_3 = upstream_model.refine_3
        self.raw_rgb_input = raw_rgb_input

        # Replicate padding exports as Pad(mode=edge), which DFC 3.x cannot map
        # to the accelerator. Zero padding has a small measured border effect and
        # is validated separately on real Nuvo calibration captures.
        self.fnet.out_conv.block[0].padding_mode = "zeros"

        self.register_buffer(
            "image_mean_255",
            torch.tensor(IMAGENET_MEAN_255).view(1, 3, 1, 1),
            persistent=False,
        )
        self.register_buffer(
            "image_std_255",
            torch.tensor(IMAGENET_STD_255).view(1, 3, 1, 1),
            persistent=False,
        )
        self.channel_mean = frozen_conv(
            FEATURE_CHANNELS, torch.full((FEATURE_CHANNELS,), 1.0 / FEATURE_CHANNELS)
        )
        self.disparity_regression = frozen_conv(
            QUARTER_DISPARITIES,
            torch.arange(QUARTER_DISPARITIES, dtype=torch.float32),
        )
        self.correlation_left_passthrough = nn.Conv2d(
            FEATURE_CHANNELS,
            FEATURE_CHANNELS,
            kernel_size=1,
            groups=FEATURE_CHANNELS,
            bias=False,
        )
        self.correlation_right_passthrough = copy.deepcopy(
            self.correlation_left_passthrough
        )
        with torch.no_grad():
            self.correlation_left_passthrough.weight.fill_(1.0)
            self.correlation_right_passthrough.weight.fill_(1.0)
        self.correlation_left_passthrough.weight.requires_grad_(False)
        self.correlation_right_passthrough.weight.requires_grad_(False)

    def normalize(self, image: torch.Tensor) -> torch.Tensor:
        if not self.raw_rgb_input:
            return image
        return (image - self.image_mean_255) / self.image_std_255

    def correlation(
        self, left_feature: torch.Tensor, right_feature: torch.Tensor
    ) -> torch.Tensor:
        branches: list[torch.Tensor] = []
        left_current = left_feature
        right_current = right_feature
        for disparity in range(QUARTER_DISPARITIES):
            if disparity == 0:
                product = left_current * right_current
                branch = self.channel_mean(product)
            else:
                product = (
                    left_current[:, :, :, disparity:]
                    * right_current[:, :, :, :-disparity]
                )
                branch = self.channel_mean(product)
                branch = F.pad(branch, (disparity, 0, 0, 0))
            branches.append(branch)
            if (
                disparity % CORRELATION_GROUP_SIZE == CORRELATION_GROUP_SIZE - 1
                and disparity + 1 < QUARTER_DISPARITIES
            ):
                left_current = self.correlation_left_passthrough(left_current)
                right_current = self.correlation_right_passthrough(right_current)
        return torch.cat(branches, dim=1)

    def context_upsample(
        self, disparity: torch.Tensor, weights: torch.Tensor
    ) -> torch.Tensor:
        high = F.interpolate(disparity * 4.0, scale_factor=4.0, mode="nearest")
        terms: list[torch.Tensor] = []
        index = 0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                shifted = shift_full_resolution(high, dy, dx)
                terms.append(shifted * weights[:, index : index + 1])
                index += 1
        return add_tree(terms)

    def forward(self, left: torch.Tensor, right: torch.Tensor) -> torch.Tensor:
        left = self.normalize(left)
        right = self.normalize(right)

        features_left = self.fnet(left)
        features_right = self.fnet_right(right) if self.fnet_right is not None else self.fnet(right)
        cost = self.correlation(features_left[0], features_right[0])
        aggregated = self.cost_agg(cost, features_left)
        probability = F.softmax(aggregated, dim=1)
        disparity = self.disparity_regression(probability)

        refine = self.refine_1(features_left[0])
        refine = self.refine_2(refine, self.stem_2(left))
        refine = self.refine_3(refine)
        upsample_weights = F.softmax(refine, dim=1)
        return self.context_upsample(disparity, upsample_weights.float())


def rewrite_wide_concats(model: onnx.ModelProto) -> int:
    """Rewrite every Concat with more than four inputs as a concat tree."""
    nodes = []
    rewritten = 0
    for node in model.graph.node:
        if node.op_type != "Concat" or len(node.input) <= MAX_CONCAT_INPUTS:
            nodes.append(node)
            continue

        rewritten += 1
        axis = next(attribute.i for attribute in node.attribute if attribute.name == "axis")
        current = list(node.input)
        level = 0
        base_name = node.name or node.output[0]
        while len(current) > MAX_CONCAT_INPUTS:
            next_level = []
            for start in range(0, len(current), MAX_CONCAT_INPUTS):
                group = current[start : start + MAX_CONCAT_INPUTS]
                output = f"{base_name}__concat_l{level}_g{start // MAX_CONCAT_INPUTS}"
                nodes.append(
                    helper.make_node("Concat", group, [output], axis=axis, name=output)
                )
                next_level.append(output)
            current = next_level
            level += 1
        nodes.append(
            helper.make_node(
                "Concat", current, list(node.output), axis=axis, name=base_name
            )
        )

    del model.graph.node[:]
    model.graph.node.extend(nodes)
    return rewritten


def graph_stats(model: onnx.ModelProto) -> dict[str, object]:
    inferred = shape_inference.infer_shapes(model)
    ranks = []
    for value in [
        *inferred.graph.input,
        *inferred.graph.value_info,
        *inferred.graph.output,
    ]:
        tensor_type = value.type.tensor_type
        if tensor_type.HasField("shape"):
            ranks.append(len(tensor_type.shape.dim))
    concat_inputs = [
        len(node.input) for node in model.graph.node if node.op_type == "Concat"
    ]
    initializer_names = {item.name for item in model.graph.initializer}
    consumers = Counter(
        input_name
        for node in model.graph.node
        for input_name in node.input
        if input_name and input_name not in initializer_names
    )
    return {
        "nodes": len(model.graph.node),
        "operators": dict(sorted(Counter(node.op_type for node in model.graph.node).items())),
        "max_rank": max(ranks),
        "rank_gt_4": sum(rank > 4 for rank in ranks),
        "max_concat_inputs": max(concat_inputs, default=0),
        "max_activation_fanout": max(consumers.values(), default=0),
        "opset": next(item.version for item in model.opset_import if item.domain == ""),
    }


def load_upstream_model(upstream: Path, checkpoint: Path) -> nn.Module:
    sys.path.insert(0, str(upstream))
    from core.models import build_model, load_model_weights  # pylint: disable=import-error

    model = build_model("las2", fnet_pretrained=False, model_size="s", max_disp=MAX_DISPARITY)
    state = torch.load(checkpoint, map_location="cpu", weights_only=True)
    load_model_weights(model, state, strict=True)
    model.eval()
    model.fnet.out_conv.block[0].padding_mode = "zeros"
    return model


def compare(reference: torch.Tensor, candidate: torch.Tensor) -> dict[str, float]:
    difference = (reference.float() - candidate.float()).abs()
    return {
        "mae": float(difference.mean()),
        "max_abs": float(difference.max()),
        "p99_abs": float(torch.quantile(difference.flatten(), 0.99)),
    }


def export(args: argparse.Namespace) -> dict[str, object]:
    global INPUT_HEIGHT, INPUT_WIDTH
    INPUT_HEIGHT = args.input_height
    INPUT_WIDTH = args.input_width
    if INPUT_HEIGHT % 32 or INPUT_WIDTH % 32:
        raise RuntimeError("LAS2 input dimensions must both be divisible by 32")
    torch.manual_seed(args.seed)
    checkpoint = args.checkpoint.resolve()
    upstream = args.upstream.resolve()
    output = args.output.resolve()
    manifest_path = args.manifest.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)

    reference = load_upstream_model(upstream, checkpoint)
    calibration_pairs = [
        (left.resolve(), right.resolve())
        for left, right in (args.freeze_instance_norm_pair or [])
    ]
    if calibration_pairs:
        frozen_source, right_fnet = freeze_instance_norms(
            copy.deepcopy(reference), calibration_pairs
        )
        candidate = HailoLas2S(
            frozen_source, raw_rgb_input=True, right_fnet=right_fnet
        ).eval()
        left = load_calibration_image(calibration_pairs[0][0])
        right = load_calibration_image(calibration_pairs[0][1])
    else:
        candidate = HailoLas2S(copy.deepcopy(reference), raw_rgb_input=True).eval()
        left = torch.rand(1, 3, INPUT_HEIGHT, INPUT_WIDTH) * 255.0
        right = torch.rand(1, 3, INPUT_HEIGHT, INPUT_WIDTH) * 255.0
    with torch.inference_mode():
        reference_output = reference(left, right, max_disp=MAX_DISPARITY, test_mode=True)
        candidate_output = candidate(left, right)
    torch_parity = compare(reference_output, candidate_output)
    if (
        torch_parity["mae"] > REWRITE_MAE_LIMIT
        or torch_parity["max_abs"] > REWRITE_MAX_ABS_LIMIT
    ):
        raise RuntimeError(f"PyTorch rewrite parity failed: {torch_parity}")

    unsimplified = output.with_suffix(".unsimplified.onnx")
    torch.onnx.export(
        candidate,
        (left, right),
        unsimplified,
        input_names=["left", "right"],
        output_names=["disparity"],
        opset_version=17,
        do_constant_folding=True,
        dynamo=False,
    )

    onnx_model = onnx.load(unsimplified)
    simplified, valid = simplify(onnx_model)
    if not valid:
        raise RuntimeError("onnxsim validation failed")
    rewritten_concats = rewrite_wide_concats(simplified)
    onnx.checker.check_model(simplified)
    onnx.save(simplified, output)
    unsimplified.unlink()

    stats = graph_stats(simplified)
    if stats["max_rank"] > 4 or stats["max_concat_inputs"] > MAX_CONCAT_INPUTS:
        raise RuntimeError(f"Exported graph violates Hailo structural gates: {stats}")

    session_options = ort.SessionOptions()
    session_options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    session = ort.InferenceSession(
        str(output), session_options, providers=["CPUExecutionProvider"]
    )
    ort_output = session.run(
        ["disparity"],
        {"left": left.numpy(), "right": right.numpy()},
    )[0]
    ort_difference = np.abs(candidate_output.numpy() - ort_output)
    ort_parity = {
        "mae": float(ort_difference.mean()),
        "max_abs": float(ort_difference.max()),
        "p99_abs": float(np.quantile(ort_difference, 0.99)),
    }
    if (
        ort_parity["mae"] > ONNX_MAE_LIMIT
        or ort_parity["max_abs"] > ONNX_MAX_ABS_LIMIT
    ):
        raise RuntimeError(f"ONNX Runtime parity failed: {ort_parity}")

    end_to_end_difference = np.abs(reference_output.numpy() - ort_output)
    end_to_end_parity = {
        "mae": float(end_to_end_difference.mean()),
        "max_abs": float(end_to_end_difference.max()),
        "p99_abs": float(np.quantile(end_to_end_difference, 0.99)),
    }
    if (
        end_to_end_parity["mae"] > END_TO_END_MAE_LIMIT
        or end_to_end_parity["max_abs"] > END_TO_END_MAX_ABS_LIMIT
    ):
        raise RuntimeError(
            f"Reference-to-ONNX Runtime parity failed: {end_to_end_parity}"
        )

    manifest: dict[str, object] = {
        "model": "LAS2-S",
        "upstream_revision": git_revision(upstream),
        "checkpoint_sha256": sha256(checkpoint),
        "onnx_sha256": sha256(output),
        "input_names": ["left", "right"],
        "input_shape_nchw": [1, 3, INPUT_HEIGHT, INPUT_WIDTH],
        "input_domain": "raw RGB float values in [0,255]",
        "output_name": "disparity",
        "output_shape_nchw": [1, 1, INPUT_HEIGHT, INPUT_WIDTH],
        "max_disparity": MAX_DISPARITY,
        "padding": (
            "replicate-pad 4 columns per side before inference; crop after inference"
            if INPUT_WIDTH == 736 and INPUT_HEIGHT == 544
            else "resize rectified stereo pair to model input; rescale disparity to output width"
        ),
        "torch_version": torch.__version__,
        "onnx_version": onnx.__version__,
        "onnxruntime_version": ort.__version__,
        "rewritten_concats": rewritten_concats,
        "graph": stats,
        "pytorch_parity": torch_parity,
        "onnxruntime_parity": ort_parity,
        "reference_to_onnxruntime_parity": end_to_end_parity,
        "frozen_instance_norm": {
            "enabled": bool(calibration_pairs),
            "calibration_pairs": len(calibration_pairs),
            "scope": "compiler feasibility only",
        },
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument(
        "--freeze-instance-norm-pair",
        type=Path,
        nargs=2,
        action="append",
        metavar=("LEFT", "RIGHT"),
        help="feasibility-only rectified pair used to freeze dynamic InstanceNorm",
    )
    parser.add_argument("--seed", type=int, default=20260711)
    parser.add_argument("--input-height", type=int, default=INPUT_HEIGHT)
    parser.add_argument("--input-width", type=int, default=INPUT_WIDTH)
    return parser.parse_args()


if __name__ == "__main__":
    result = export(parse_args())
    print(json.dumps(result, indent=2, sort_keys=True))
