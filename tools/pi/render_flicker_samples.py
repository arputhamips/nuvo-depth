#!/usr/bin/env python3
"""Render aligned Pi ISP/rectified/disparity examples and a luma trace."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import cv2
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "sdk/python"))
from nuvo_depth import decode_frame


def render_pair(trace: Path, output: Path, name: str, row: dict) -> dict:
    sequence = row["capture_sequence"]
    frame = decode_frame((trace / f"post_depth_{sequence}.nuv").read_bytes())
    rectified = frame.products["left_rectified"]
    gray = np.frombuffer(rectified.data, dtype=np.uint8).reshape(
        rectified.height, rectified.width)
    disparity = frame.products["disparity"]
    values = np.frombuffer(disparity.data, dtype="<f4").reshape(
        disparity.height, disparity.width)
    valid = np.isfinite(values) & (values > 0) & (values < 128)
    if "validity" in frame.products:
        product = frame.products["validity"]
        valid &= np.frombuffer(product.data, dtype=np.uint8).reshape(
            product.height, product.width) != 0
    display_values = np.nan_to_num(values, nan=0.0, posinf=128.0, neginf=0.0)
    colors = cv2.applyColorMap(np.uint8(np.clip(display_values, 0, 128) * (255 / 128)),
                               cv2.COLORMAP_TURBO)
    colors[~valid] = 0
    cv2.imwrite(str(output / f"{name}_rectified_{sequence}.png"), gray)
    cv2.imwrite(str(output / f"{name}_disparity_{sequence}.png"), colors)
    return {"capture_sequence": sequence, "isp_luma": row["isp_left"],
            "valid_disparity_fraction": float(valid.mean()),
            "disparity_p50_px": float(np.median(values[valid])) if valid.any() else None}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rows = json.loads(args.report.read_text())["frames"]
    ordered = sorted(rows, key=lambda row: row["isp_left"])
    selected = {"dark": ordered[0], "median": ordered[len(ordered) // 2],
                "bright": ordered[-1]}
    details = {name: render_pair(args.trace, args.output, name, row)
               for name, row in selected.items()}

    times = np.array([row["left_sensor_timestamp_ns"] for row in rows],
                     dtype=np.float64)
    times = (times - times[0]) / 1e9
    fig, axes = plt.subplots(2, 1, figsize=(11, 6), sharex=True,
                             gridspec_kw={"height_ratios": [3, 1]})
    for key, label in (("isp_left", "ISP left"), ("isp_right", "ISP right"),
                       ("rectified_left", "Rectified left"),
                       ("pc_stream", "PC H.264 decode")):
        axes[0].plot(times, [row[key] for row in rows], marker=".",
                     linewidth=1, label=label)
    axes[0].set_ylabel("Central ROI mean luma (0–255)")
    axes[0].legend(loc="upper right", ncol=2)
    axes[0].grid(alpha=0.3)
    axes[1].plot(times, [row["left_analogue_gain"] for row in rows],
                 marker=".", linewidth=1, label="Analogue gain")
    axes[1].plot(times, [row["left_digital_gain"] for row in rows],
                 marker=".", linewidth=1, label="Digital gain")
    axes[1].set_ylabel("Gain")
    axes[1].set_xlabel("Seconds from first captured frame")
    axes[1].legend(loc="upper right", ncol=2)
    axes[1].grid(alpha=0.3)
    fig.suptitle("60 aligned frames · fixed 7,999 µs exposure · 728×544")
    fig.tight_layout()
    fig.savefig(args.output / "luma_stages.png", dpi=160)
    plt.close(fig)
    (args.output / "samples.json").write_text(json.dumps(details, indent=2) + "\n")
    print(json.dumps(details, indent=2))


if __name__ == "__main__":
    main()
