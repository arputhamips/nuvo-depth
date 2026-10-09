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
from analyze_flicker_trace import decoded_image


def render_pair(trace: Path, pc_trace: Path, output: Path, name: str,
                row: dict, max_disparity: float) -> dict:
    sequence = row["capture_sequence"]
    frame = decode_frame((trace / f"post_depth_{sequence}.nuv").read_bytes())
    for side in ("left","right"):
        isp=cv2.imread(str(trace/f"isp_{side}_{sequence}.png"),cv2.IMREAD_GRAYSCALE)
        if isp is None: raise ValueError(f"missing ISP {side} frame {sequence}")
        cv2.imwrite(str(output/f"{name}_isp_{side}_{sequence}.png"),isp)
        rectified=frame.products[f"{side}_rectified"]
        gray=np.frombuffer(rectified.data,dtype=np.uint8).reshape(
            rectified.height,rectified.width)
        cv2.imwrite(str(output/f"{name}_rectified_{side}_{sequence}.png"),gray)
    disparity = frame.products["disparity"]
    values = np.frombuffer(disparity.data, dtype="<f4").reshape(
        disparity.height, disparity.width)
    valid = np.isfinite(values) & (values > 0) & (values < max_disparity)
    if "validity" in frame.products:
        product = frame.products["validity"]
        valid &= np.frombuffer(product.data, dtype=np.uint8).reshape(
            product.height, product.width) != 0
    display_values = np.nan_to_num(values, nan=0.0, posinf=max_disparity, neginf=0.0)
    colors = cv2.applyColorMap(np.uint8(np.clip(display_values, 0, max_disparity) * (255 / max_disparity)),
                               cv2.COLORMAP_TURBO)
    colors[~valid] = 0
    cv2.imwrite(str(output / f"{name}_disparity_{sequence}.png"), colors)
    for stage,path in (("post_encoding",trace/f"post_encoding_{sequence}.nve"),
                       ("pc_stream",pc_trace/f"pc_stream_{sequence}.nve")):
        _,image=decoded_image(path)
        cv2.imwrite(str(output/f"{name}_{stage}_{sequence}.png"),image)
    source_side=row.get("encoding_source","left_rectified").split("_",1)[0]
    if source_side not in {"left","right"}: source_side="left"
    return {"capture_sequence": sequence, "isp_luma": row[f"isp_{source_side}"],
            "valid_disparity_fraction": float(valid.mean()),
            "disparity_p50_px": float(np.median(values[valid])) if valid.any() else None}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--pc-trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-disparity", type=float, default=128.0)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report=json.loads(args.report.read_text())
    rows=report["frames"]
    sources=set(report["summary"]["encoding_sources"])
    side="right" if sources=={"right_rectified"} else "left"
    ordered = sorted(rows, key=lambda row: row[f"isp_{side}"])
    selected = {"dark": ordered[0], "median": ordered[len(ordered) // 2],
                "bright": ordered[-1]}
    details = {name: render_pair(args.trace, args.pc_trace, args.output, name,
                                 row, args.max_disparity)
               for name, row in selected.items()}
    montage_rows=[]
    for name in ("dark","bright"):
        sequence=selected[name]["capture_sequence"]
        tiles=[]
        for stage,label in ((f"isp_{side}",f"ISP {side}"),
                            (f"rectified_{side}","Rectified"),
                            ("disparity","Stereo disparity"),
                            ("post_encoding","Pi H.264"),
                            ("pc_stream","PC stream")):
            image=cv2.imread(str(args.output/f"{name}_{stage}_{sequence}.png"))
            if image is None: raise ValueError(f"missing montage stage {stage}")
            tile=np.zeros((272,320,3),dtype=np.uint8)
            tile[32:]=cv2.resize(image,(320,240),interpolation=cv2.INTER_AREA)
            cv2.putText(tile,f"{name} #{sequence} | {label}",(6,21),
                        cv2.FONT_HERSHEY_SIMPLEX,0.45,(255,255,255),1,cv2.LINE_AA)
            tiles.append(tile)
        montage_rows.append(np.hstack(tiles))
    cv2.imwrite(str(args.output/"dark_vs_bright_stages.png"),
                np.vstack(montage_rows))

    times = np.array([row["left_sensor_timestamp_ns"] for row in rows],
                     dtype=np.float64)
    times = (times - times[0]) / 1e9
    fig, axes = plt.subplots(3, 1, figsize=(11, 8), sharex=True,
                             gridspec_kw={"height_ratios": [3, 1, 1]})
    for key, label in ((f"isp_{side}", f"ISP {side}"),
                       (f"rectified_{side}", f"Rectified {side}"),
                       ("post_encoding", "Pi H.264 decode"),
                       ("pc_stream", "PC H.264 decode")):
        axes[0].plot(times, [row[key] for row in rows], marker=".",
                     linewidth=1, label=label)
    axes[0].set_ylabel("Central ROI mean luma (0–255)")
    axes[0].legend(loc="upper right", ncol=2)
    axes[0].grid(alpha=0.3)
    axes[1].plot(times, [row[f"{side}_analogue_gain"] for row in rows],
                 marker=".", linewidth=1, label="Analogue gain")
    axes[1].plot(times, [row[f"{side}_digital_gain"] for row in rows],
                 marker=".", linewidth=1, label="Digital gain")
    axes[1].set_ylabel("Gain")
    axes[1].legend(loc="upper right", ncol=2)
    axes[1].grid(alpha=0.3)
    axes[2].plot(times, [row["disparity_valid_fraction"] for row in rows],
                 marker=".", linewidth=1, color="tab:green")
    axes[2].set_ylabel("Valid disparity")
    axes[2].grid(alpha=0.3)
    axes[2].set_ylim(0, 1)
    axes[2].set_xlabel("Seconds from first captured frame")
    fig.suptitle("60 aligned frames · reported exposure 7,999 µs · 728×544")
    fig.tight_layout()
    fig.savefig(args.output / "luma_stages.png", dpi=160)
    plt.close(fig)
    (args.output / "samples.json").write_text(json.dumps(details, indent=2) + "\n")
    print(json.dumps(details, indent=2))


if __name__ == "__main__":
    main()
