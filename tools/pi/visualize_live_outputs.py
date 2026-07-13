#!/usr/bin/env python3
"""Render human-readable artifacts from a completed Depth Daemon live run."""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

import cv2
import numpy as np


MAGIC = 0x314D455344505444
SCHEMA_VERSION = 1
ENDIAN_TAG = 0x01020304
HEADER_BYTES = 4096
SLOT_HEADER_BYTES = 4096
SLOT_COUNT = 4
OBJECT_RECORD_BYTES = 256
OBJECT_CAPACITY = 64
PREVIEW_CAPACITY_BYTES = 512 * 1024
OBJECT_OFFSET = SLOT_HEADER_BYTES
PREVIEW_OFFSET = OBJECT_OFFSET + OBJECT_CAPACITY * OBJECT_RECORD_BYTES
SLOT_STRIDE_BYTES = PREVIEW_OFFSET + PREVIEW_CAPACITY_BYTES
FILE_BYTES = HEADER_BYTES + SLOT_COUNT * SLOT_STRIDE_BYTES

CLASS_NAMES = {
    0: "unknown",
    1: "human",
    2: "animal",
    3: "vegetation",
    4: "wall",
    5: "fence",
    6: "terrain",
    7: "generic_obstacle",
}
CLASS_COLOURS = {
    0: (180, 180, 180),
    1: (30, 30, 235),
    2: (220, 30, 220),
    3: (40, 190, 40),
    4: (20, 140, 245),
    5: (220, 210, 20),
    6: (70, 130, 180),
    7: (20, 220, 240),
}


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def _i64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<q", data, offset)[0]


def _f32(data: bytes, offset: int) -> float:
    return struct.unpack_from("<f", data, offset)[0]


def _c_string(data: bytes, offset: int, capacity: int, name: str) -> str:
    raw = data[offset : offset + capacity]
    terminator = raw.find(b"\0")
    if terminator < 0:
        raise ValueError(f"{name} is not NUL-terminated")
    return raw[:terminator].decode("utf-8", errors="strict")


def _finite_or_none(value: float) -> float | None:
    return value if math.isfinite(value) else None


def read_semantic_snapshot(path: Path) -> tuple[dict[str, object], bytes]:
    data = path.read_bytes()
    if len(data) != FILE_BYTES:
        raise ValueError(
            f"semantic ring size {len(data)} does not match {FILE_BYTES}"
        )
    expected_header = (
        MAGIC,
        SCHEMA_VERSION,
        ENDIAN_TAG,
        HEADER_BYTES,
        SLOT_HEADER_BYTES,
        SLOT_COUNT,
        SLOT_STRIDE_BYTES,
        OBJECT_RECORD_BYTES,
        OBJECT_CAPACITY,
        OBJECT_OFFSET,
        PREVIEW_OFFSET,
        PREVIEW_CAPACITY_BYTES,
        FILE_BYTES,
    )
    actual_header = (
        _u64(data, 0),
        _u32(data, 8),
        _u32(data, 12),
        _u32(data, 16),
        _u32(data, 20),
        _u32(data, 24),
        _u32(data, 28),
        _u32(data, 32),
        _u32(data, 36),
        _u32(data, 40),
        _u32(data, 44),
        _u32(data, 48),
        _u64(data, 56),
    )
    if actual_header != expected_header:
        raise ValueError("semantic ring header does not match schema v1")

    instance_id = _u64(data, 64)
    sequence = _u64(data, 88)
    if sequence == 0:
        raise ValueError("semantic ring has no complete result")
    slot = HEADER_BYTES + (sequence % SLOT_COUNT) * SLOT_STRIDE_BYTES
    begin_guard = _u64(data, slot)
    end_guard = _u64(data, slot + 8)
    slot_sequence = _u64(data, slot + 16)
    slot_instance = _u64(data, slot + 24)
    committed_guard = sequence * 2
    if (
        begin_guard != committed_guard
        or end_guard != committed_guard
        or slot_sequence != sequence
        or slot_instance != instance_id
    ):
        raise ValueError("semantic ring latest slot is torn or inconsistent")

    object_count = _u32(data, slot + 64)
    instance_count = _u32(data, slot + 68)
    dense_region_count = _u32(data, slot + 72)
    preview_bytes = _u32(data, slot + 76)
    model_count = _u32(data, slot + 80)
    if object_count > OBJECT_CAPACITY or model_count > 4:
        raise ValueError("semantic ring fixed capacity exceeded")
    if instance_count + dense_region_count != object_count:
        raise ValueError("semantic object counts are inconsistent")
    if preview_bytes > PREVIEW_CAPACITY_BYTES:
        raise ValueError("semantic preview capacity exceeded")

    hazard_offset = slot + 256
    hazard = {
        "active": bool(_u32(data, hazard_offset)),
        "perception_stale": bool(_u32(data, hazard_offset + 4)),
        "fail_closed_required": bool(_u32(data, hazard_offset + 8)),
        "source_observation_id": _u32(data, hazard_offset + 12),
        "semantic_class": CLASS_NAMES.get(
            struct.unpack_from("<H", data, hazard_offset + 16)[0], "invalid"
        ),
        "range_m": _finite_or_none(_f32(data, hazard_offset + 20)),
        "bearing_rad": _finite_or_none(_f32(data, hazard_offset + 24)),
        "reason": _c_string(data, hazard_offset + 28, 64, "hazard reason"),
    }

    models: list[dict[str, object]] = []
    for index in range(model_count):
        model = slot + 384 + index * 256
        models.append(
            {
                "name": _c_string(data, model, 48, "model name"),
                "version": _c_string(data, model + 48, 32, "model version"),
                "sha256": _c_string(data, model + 80, 65, "model SHA256"),
                "health": _u32(data, model + 148),
                "completed_runs": _u64(data, model + 160),
                "errors": _u64(data, model + 168),
                "latency_ms": _f32(data, model + 176),
                "inference_latency_ms": _f32(data, model + 180),
                "postprocess_latency_ms": _f32(data, model + 184),
            }
        )

    objects: list[dict[str, object]] = []
    for index in range(object_count):
        record = slot + OBJECT_OFFSET + index * OBJECT_RECORD_BYTES
        semantic_class = struct.unpack_from("<H", data, record + 2)[0]
        objects.append(
            {
                "source_kind": "instance" if data[record] == 1 else "dense_region",
                "range_valid": bool(data[record + 1]),
                "semantic_class_id": semantic_class,
                "semantic_class": CLASS_NAMES.get(semantic_class, "invalid"),
                "observation_id": _u32(data, record + 4),
                "model_class_id": _u32(data, record + 8),
                "pixel_count": _u32(data, record + 12),
                "confidence": _f32(data, record + 20),
                "box": [
                    _f32(data, record + 24),
                    _f32(data, record + 28),
                    _f32(data, record + 32),
                    _f32(data, record + 36),
                ],
                "point": [
                    _finite_or_none(_f32(data, record + 40)),
                    _finite_or_none(_f32(data, record + 44)),
                    _finite_or_none(_f32(data, record + 48)),
                ],
                "range_m": _finite_or_none(_f32(data, record + 52)),
                "bearing_rad": _finite_or_none(_f32(data, record + 56)),
                "range_stddev_m": _finite_or_none(_f32(data, record + 60)),
                "valid_fraction": _f32(data, record + 64),
                "valid_samples": _u32(data, record + 68),
                "model_name": _c_string(data, record + 72, 48, "object model"),
                "model_label": _c_string(data, record + 120, 32, "object label"),
            }
        )

    preview_start = slot + PREVIEW_OFFSET
    preview = data[preview_start : preview_start + preview_bytes]
    snapshot: dict[str, object] = {
        "sequence": sequence,
        "source_sequence": _u64(data, slot + 32),
        "capture_realtime_ns": _i64(data, slot + 40),
        "ready_realtime_ns": _i64(data, slot + 48),
        "health": _u32(data, slot + 56),
        "target_frame": _c_string(data, slot + 136, 64, "target frame"),
        "object_count": object_count,
        "instance_count": instance_count,
        "dense_region_count": dense_region_count,
        "submitted_frames": _u64(data, slot + 88),
        "replaced_pending_frames": _u64(data, slot + 96),
        "processed_frames": _u64(data, slot + 104),
        "published_results": _u64(data, slot + 112),
        "worker_errors": _u64(data, slot + 120),
        "frame_latency_ms": _f32(data, slot + 128),
        "max_frame_latency_ms": _f32(data, slot + 132),
        "hazard": hazard,
        "models": models,
        "objects": objects,
    }
    return snapshot, preview


def draw_semantic_overlay(image: np.ndarray, snapshot: dict[str, object]) -> np.ndarray:
    output = image.copy()
    for item in snapshot["objects"]:
        assert isinstance(item, dict)
        class_id = int(item["semantic_class_id"])
        colour = CLASS_COLOURS.get(class_id, CLASS_COLOURS[0])
        x0, y0, x1, y1 = (round(float(value)) for value in item["box"])
        thickness = 3 if item["source_kind"] == "instance" else 2
        cv2.rectangle(output, (x0, y0), (x1, y1), colour, thickness)
        label = f"{item['semantic_class']} {float(item['confidence']):.2f}"
        if item["range_valid"] and item["range_m"] is not None:
            label += f" {float(item['range_m']):.2f}m"
        (width, height), _ = cv2.getTextSize(
            label, cv2.FONT_HERSHEY_SIMPLEX, 0.48, 1
        )
        top = max(0, y0 - height - 8)
        cv2.rectangle(output, (x0, top), (x0 + width + 6, y0), colour, -1)
        cv2.putText(
            output,
            label,
            (x0 + 3, max(height + 1, y0 - 4)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.48,
            (0, 0, 0),
            1,
            cv2.LINE_AA,
        )
    hazard = snapshot["hazard"]
    assert isinstance(hazard, dict)
    banner_colour = (30, 30, 220) if hazard["active"] else (40, 150, 40)
    text = "HAZARD" if hazard["active"] else "CLEAR"
    text += f" | {hazard['reason']}"
    cv2.rectangle(output, (0, 0), (output.shape[1], 30), banner_colour, -1)
    cv2.putText(
        output,
        text,
        (8, 21),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        (255, 255, 255),
        1,
        cv2.LINE_AA,
    )
    return output


def colourize_disparity(disparity: np.ndarray) -> np.ndarray:
    valid = np.isfinite(disparity) & (disparity > 0.0)
    scaled = np.zeros(disparity.shape, dtype=np.uint8)
    if np.any(valid):
        low, high = np.percentile(disparity[valid], [2.0, 98.0])
        if high <= low:
            high = low + 1.0
        scaled[valid] = np.clip(
            (disparity[valid] - low) * 255.0 / (high - low), 0.0, 255.0
        ).astype(np.uint8)
    colour = cv2.applyColorMap(scaled, cv2.COLORMAP_TURBO)
    colour[~valid] = 0
    return colour


def labelled_panel(image: np.ndarray, label: str) -> np.ndarray:
    panel = image.copy()
    cv2.rectangle(panel, (0, 0), (panel.shape[1], 30), (25, 25, 25), -1)
    cv2.putText(
        panel,
        label,
        (8, 21),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.55,
        (255, 255, 255),
        1,
        cv2.LINE_AA,
    )
    return panel


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    args = parser.parse_args()
    run_dir = args.run_dir
    snapshot, preview_bytes = read_semantic_snapshot(run_dir / "semantic.ring")
    if not preview_bytes:
        raise RuntimeError("semantic preview is absent; rerun with --semantic-preview")
    preview = cv2.imdecode(np.frombuffer(preview_bytes, np.uint8), cv2.IMREAD_COLOR)
    if preview is None:
        raise RuntimeError("semantic preview JPEG could not be decoded")
    disparity = np.load(run_dir / "disparity_float32.npy")
    left = cv2.imread(str(run_dir / "left_rect.png"), cv2.IMREAD_GRAYSCALE)
    right = cv2.imread(str(run_dir / "right_rect.png"), cv2.IMREAD_GRAYSCALE)
    if left is None or right is None:
        raise RuntimeError("rectified stereo images are missing")

    overlay = draw_semantic_overlay(preview, snapshot)
    disparity_colour = colourize_disparity(disparity)
    preview_path = run_dir / "semantic_preview.jpg"
    overlay_path = run_dir / "semantic_overlay.jpg"
    disparity_path = run_dir / "disparity_colormap.png"
    cv2.imwrite(str(preview_path), preview)
    cv2.imwrite(str(overlay_path), overlay)
    cv2.imwrite(str(disparity_path), disparity_colour)

    stereo = np.hstack(
        (
            labelled_panel(cv2.cvtColor(left, cv2.COLOR_GRAY2BGR), "LEFT RECTIFIED"),
            labelled_panel(cv2.cvtColor(right, cv2.COLOR_GRAY2BGR), "RIGHT RECTIFIED"),
        )
    )
    products = np.hstack(
        (
            labelled_panel(disparity_colour, "DISPARITY (nearer = warmer)"),
            overlay,
        )
    )
    montage = np.vstack((stereo, products))
    cv2.imwrite(str(run_dir / "results_montage.jpg"), montage)
    (run_dir / "semantic_snapshot.json").write_text(
        json.dumps(snapshot, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(snapshot, indent=2))


if __name__ == "__main__":
    main()
