#!/usr/bin/env python3
"""Measure current flicker in PC-received NVE packets without a Pi trace."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from analyze_flicker_trace import encoded_luma


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pc-trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-non-camera-source", action="store_true")
    args = parser.parse_args()
    rows = []
    for packet in sorted(args.pc_trace.glob("pc_stream_*.nve"), key=lambda p: int(p.stem.rsplit("_", 1)[1])):
        sequence, luma, source = encoded_luma(packet)
        rows.append({"capture_sequence": sequence, "central_luma": luma, "source": source})
    if len(rows) < 2:
        raise SystemExit("at least two decodable PC packets are required")
    sources = {row["source"] for row in rows}
    if not args.allow_non_camera_source and not sources <= {"left_rectified", "right_rectified"}:
        raise SystemExit(f"cannot diagnose brightness from non-camera source: {sorted(sources)}")
    values = np.array([row["central_luma"] for row in rows], dtype=float)
    lag1 = float(np.corrcoef(values[:-1], values[1:])[0, 1]) if values.std() > 0 else None
    result = {
        "count": len(rows),
        "sources": sorted(sources),
        "central_luma_min": float(values.min()),
        "central_luma_median": float(np.median(values)),
        "central_luma_max": float(values.max()),
        "central_luma_std": float(values.std()),
        "max_over_min": float(values.max() / values.min()) if values.min() > 0 else None,
        "adjacent_output_luma_correlation": lag1,
        "adjacent_output_rms_change": float(np.sqrt(np.mean(np.diff(values) ** 2))),
        "note": (
            "This PC-only camera-source check can measure current visible brightness flicker, "
            "but cannot locate its source. See the aligned Pi/PC trace for attribution."
            if sources <= {"left_rectified", "right_rectified"}
            else "This stream contains a non-camera product; its luma is not evidence about ISP exposure flicker."
        ),
        "frames": rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "frames"}, indent=2))


if __name__ == "__main__":
    main()
