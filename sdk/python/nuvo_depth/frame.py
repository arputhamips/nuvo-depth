"""NuvoStream v1 framing with strict allocation and product bounds."""

from __future__ import annotations

from dataclasses import dataclass
import json
import struct
from types import MappingProxyType
from typing import Any, Mapping

HEADER = struct.Struct(">4sHHHHIQQ")
MAX_METADATA = 64 * 1024
MAX_PAYLOAD = 64 * 1024 * 1024


@dataclass(frozen=True)
class Product:
    kind: str
    format: str
    width: int
    height: int
    data: memoryview

    def float32_le(self) -> memoryview:
        if self.format != "float32_le" or len(self.data) % 4:
            raise TypeError(f"{self.kind} is not a float32 product")
        return self.data.cast("f")


@dataclass(frozen=True)
class FrameSet:
    stream_sequence: int
    capture_epoch: int
    capture_sequence: int
    capture_time_ns: int
    configuration_epoch: int
    calibration_sha256: str
    calibration_authority: int
    metadata: Mapping[str, Any]
    products: Mapping[str, Product]
    _wire_bytes: bytes


def decode_frame(wire_bytes: bytes) -> FrameSet:
    if len(wire_bytes) < HEADER.size:
        raise ValueError("NuvoStream message is shorter than its header")
    magic, major, minor, kind, flags, metadata_size, payload_size, sequence = (
        HEADER.unpack_from(wire_bytes)
    )
    if magic != b"NUV1" or major != 1 or minor > 0 or kind != 5 or flags != 0:
        raise ValueError("unsupported or invalid NuvoStream frame header")
    if not 0 < metadata_size <= MAX_METADATA or not 0 < payload_size <= MAX_PAYLOAD:
        raise ValueError("NuvoStream frame exceeds hard bounds")
    expected = HEADER.size + metadata_size + payload_size
    if len(wire_bytes) != expected or sequence == 0:
        raise ValueError("NuvoStream frame length or sequence is invalid")
    metadata_end = HEADER.size + metadata_size
    try:
        metadata = json.loads(wire_bytes[HEADER.size:metadata_end])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("NuvoStream metadata is not valid UTF-8 JSON") from error
    if metadata.get("schema_version") != 1 or not isinstance(metadata.get("products"), list):
        raise ValueError("unsupported Nuvo frame metadata schema")
    payload = memoryview(wire_bytes)[metadata_end:]
    products: dict[str, Product] = {}
    occupied: list[tuple[int, int]] = []
    for descriptor in metadata["products"]:
        try:
            name = descriptor["kind"]
            data_format = descriptor["format"]
            offset = int(descriptor["offset"])
            length = int(descriptor["length"])
            width = int(descriptor["width"])
            height = int(descriptor["height"])
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError("malformed Nuvo product descriptor") from error
        if (
            not isinstance(name, str)
            or name in products
            or data_format not in {"gray8", "uint8", "float32_le"}
            or offset < 0
            or length <= 0
            or width <= 0
            or height <= 0
            or offset + length > payload_size
        ):
            raise ValueError("invalid or duplicate Nuvo product descriptor")
        element_size = 4 if data_format == "float32_le" else 1
        if length != width * height * element_size:
            raise ValueError("Nuvo product length disagrees with its grid")
        if any(offset < end and offset + length > start for start, end in occupied):
            raise ValueError("Nuvo product payload ranges overlap")
        occupied.append((offset, offset + length))
        products[name] = Product(
            name, data_format, width, height, payload[offset : offset + length]
        )
    required = {
        "capture_epoch",
        "capture_sequence",
        "capture_time_ns",
        "configuration_epoch",
        "calibration_sha256",
        "calibration_authority",
    }
    if not required.issubset(metadata):
        raise ValueError("Nuvo frame identity metadata is incomplete")
    return FrameSet(
        stream_sequence=sequence,
        capture_epoch=int(metadata["capture_epoch"]),
        capture_sequence=int(metadata["capture_sequence"]),
        capture_time_ns=int(metadata["capture_time_ns"]),
        configuration_epoch=int(metadata["configuration_epoch"]),
        calibration_sha256=str(metadata["calibration_sha256"]),
        calibration_authority=int(metadata["calibration_authority"]),
        metadata=MappingProxyType(metadata),
        products=MappingProxyType(products),
        _wire_bytes=wire_bytes,
    )
