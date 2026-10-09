#!/usr/bin/env python3
"""Extract one NUV1 frame set into lossless analysis products."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import cv2
import numpy as np


def parse_packet(path: Path) -> tuple[dict, memoryview]:
    data = path.read_bytes()
    if len(data) < 32 or data[:4] != b"NUV1":
        raise ValueError("input is not a NUV1 frame")
    metadata_bytes = struct.unpack_from(">I", data, 12)[0]
    payload_bytes = struct.unpack_from(">Q", data, 16)[0]
    if metadata_bytes > 65536 or payload_bytes > 64 * 1024 * 1024:
        raise ValueError("NUV1 declared lengths exceed supported limits")
    if 32 + metadata_bytes + payload_bytes != len(data):
        raise ValueError("NUV1 declared lengths do not match the file")
    metadata = json.loads(data[32 : 32 + metadata_bytes].decode("utf-8"))
    return metadata, memoryview(data)[32 + metadata_bytes :]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("packet", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    metadata, payload = parse_packet(args.packet)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    for product in metadata.get("products", []):
        width = int(product["width"])
        height = int(product["height"])
        offset = int(product["offset"])
        length = int(product["length"])
        source = payload[offset : offset + length]
        kind = product["kind"]
        if product["format"] == "gray8":
            array = np.frombuffer(source, np.uint8).reshape(height, width)
            if not cv2.imwrite(str(args.output / f"{kind}.png"), array):
                raise RuntimeError(f"could not write {kind}.png")
        elif product["format"] == "float32_le":
            array = np.frombuffer(source, "<f4").reshape(height, width).copy()
            np.save(args.output / f"{kind}.npy", array)
        elif product["format"] == "uint8":
            array = np.frombuffer(source, np.uint8).reshape(height, width).copy()
            np.save(args.output / f"{kind}.npy", array)
        else:
            (args.output / f"{kind}.bin").write_bytes(source)
    print(json.dumps({"capture_sequence": metadata.get("capture_sequence"),
                      "products": len(metadata.get("products", []))}))


if __name__ == "__main__":
    main()
