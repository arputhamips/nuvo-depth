#!/usr/bin/env python3
"""Compare aligned ISP, post-depth, post-encode, and PC WebSocket frames."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

import cv2
import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"sdk/python"))
from nuvo_depth import decode_encoded_frame, decode_frame


def mean_luma(image: np.ndarray) -> float:
    height,width=image.shape[:2]
    roi=image[height//5:height*4//5,width//5:width*4//5]
    if roi.ndim==3: roi=cv2.cvtColor(roi,cv2.COLOR_BGR2GRAY)
    return float(np.mean(roi))


def encoded_luma(path: Path) -> tuple[int,float,str]:
    encoded=decode_encoded_frame(path.read_bytes())
    payload=bytes(encoded.data)
    if encoded.codec=="jpeg":
        image=cv2.imdecode(np.frombuffer(payload,dtype=np.uint8),cv2.IMREAD_GRAYSCALE)
    elif encoded.codec=="h264":
        with tempfile.TemporaryDirectory() as directory:
            bitstream=Path(directory)/"frame.h264"
            bitstream.write_bytes(payload)
            decoder=cv2.VideoCapture(str(bitstream))
            success,image=decoder.read()
            decoder.release()
            if not success: image=None
    else: raise ValueError(f"unknown codec {encoded.codec}")
    if image is None: raise ValueError(f"could not decode {path.name}")
    return encoded.capture_sequence,mean_luma(image),str(encoded.metadata["source"])


def correlation(rows: list[dict], first: str, second: str) -> dict:
    paired=[(row[first],row[second]) for row in rows
            if first in row and second in row]
    if len(paired)<2: return {"pairs":len(paired),"correlation":None,"mean_absolute_difference":None}
    a,b=np.asarray(paired,dtype=float).T
    return {"pairs":len(paired),
            "correlation":float(np.corrcoef(a,b)[0,1]) if np.std(a)>0 and np.std(b)>0 else None,
            "mean_absolute_difference":float(np.mean(np.abs(a-b)))}


def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pi-trace",type=Path,required=True)
    parser.add_argument("--pc-trace",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    rows=[]
    for metadata_file in sorted(args.pi_trace.glob("isp_metadata_*.json"),
                                key=lambda path: int(path.stem.rsplit("_", 1)[1])):
        metadata=json.loads(metadata_file.read_text())
        sequence=metadata["capture_sequence"]
        row={"capture_sequence":sequence,
             "sensor_delta_ns":metadata["sensor_delta_ns"],
             "left_sensor_timestamp_ns":metadata["left"]["sensor_timestamp_ns"],
             "right_sensor_timestamp_ns":metadata["right"]["sensor_timestamp_ns"],
             "left_exposure_us":metadata["left"]["exposure_us"],
             "right_exposure_us":metadata["right"]["exposure_us"],
             "left_analogue_gain":metadata["left"]["analogue_gain"],
             "right_analogue_gain":metadata["right"]["analogue_gain"],
             "left_digital_gain":metadata["left"]["digital_gain"],
             "right_digital_gain":metadata["right"]["digital_gain"]}
        for side in ("left","right"):
            image=cv2.imread(str(args.pi_trace/f"isp_{side}_{sequence}.png"),cv2.IMREAD_GRAYSCALE)
            if image is not None: row[f"isp_{side}"]=mean_luma(image)
        depth_file=args.pi_trace/f"post_depth_{sequence}.nuv"
        if depth_file.exists():
            depth=decode_frame(depth_file.read_bytes())
            for side in ("left","right"):
                product=depth.products[f"{side}_rectified"]
                image=np.frombuffer(product.data,dtype=np.uint8).reshape(product.height,product.width)
                row[f"rectified_{side}"]=mean_luma(image)
            row["depth_width"]=depth.products["disparity"].width
            row["depth_height"]=depth.products["disparity"].height
        pi_encoded=args.pi_trace/f"post_encoding_{sequence}.nve"
        pc_encoded=args.pc_trace/f"pc_stream_{sequence}.nve"
        if pi_encoded.exists():
            try:
                _,row["post_encoding"],row["encoding_source"]=encoded_luma(pi_encoded)
            except ValueError as error:
                row["post_encoding_decode_error"]=str(error)
        if pc_encoded.exists():
            try:
                _,row["pc_stream"],row["pc_stream_source"]=encoded_luma(pc_encoded)
            except ValueError as error:
                row["pc_stream_decode_error"]=str(error)
        if pi_encoded.exists() and pc_encoded.exists():
            row["stream_bytes_identical"]=(hashlib.sha256(pi_encoded.read_bytes()).digest()==
                                           hashlib.sha256(pc_encoded.read_bytes()).digest())
        rows.append(row)
    summary={"isp_pairs":len(rows),
             "post_depth_pairs":sum("rectified_left" in row for row in rows),
             "post_encoding_pairs":sum("post_encoding" in row for row in rows),
             "pc_stream_pairs":sum("pc_stream" in row for row in rows),
             "depth_grids":sorted({(row["depth_width"],row["depth_height"])
                                   for row in rows if "depth_width" in row}),
             "isp_to_rectified":correlation(rows,"isp_left","rectified_left"),
             "left_to_right_isp":correlation(rows,"isp_left","isp_right"),
             "rectified_to_encoded_left":correlation(
                 [row for row in rows if row.get("encoding_source")=="left_rectified"],
                 "rectified_left","post_encoding"),
             "rectified_to_encoded_right":correlation(
                 [row for row in rows if row.get("encoding_source")=="right_rectified"],
                 "rectified_right","post_encoding"),
             "encoded_to_pc":correlation(rows,"post_encoding","pc_stream"),
             "digital_gain_to_isp":correlation(rows,"left_digital_gain","isp_left"),
             "analogue_gain_to_isp":correlation(rows,"left_analogue_gain","isp_left"),
             "stream_packets_identical":sum(row.get("stream_bytes_identical",False) for row in rows),
             "stream_packets_compared":sum("stream_bytes_identical" in row for row in rows)}
    summary["encoding_sources"]=sorted({row["encoding_source"] for row in rows
                                        if "encoding_source" in row})
    summary["encoded_decode_failures"]=sum("post_encoding_decode_error" in row
                                           for row in rows)
    for key in ("isp_left","rectified_left","post_encoding","pc_stream",
                "left_exposure_us","left_analogue_gain","left_digital_gain"):
        values=[row[key] for row in rows if key in row]
        if values: summary[key]={"min":min(values),"max":max(values),
                                 "mean":float(np.mean(values)),"std":float(np.std(values))}
    report={"summary":summary,"frames":rows}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(summary,indent=2))


if __name__=="__main__": main()
