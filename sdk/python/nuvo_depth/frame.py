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
PRODUCT_FORMATS = {
    "left_rectified": {"gray8", "bgr8", "rgb8"},
    "right_rectified": {"gray8", "bgr8", "rgb8"},
    "disparity": {"float32_le"}, "depth": {"float32_le"},
    "confidence": {"uint8"}, "validity": {"uint8"},
    "left_mono": {"gray8"}, "right_mono": {"gray8"},
    "left_rgb": {"rgb8"}, "right_rgb": {"rgb8"},
    "left_rectified_rgb": {"rgb8"}, "right_rectified_rgb": {"rgb8"},
    "rectified_stereo": {"gray8"}, "rectified_stereo_rgb": {"rgb8"},
}


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

    def to_bytes(self) -> bytes:
        """Return the original immutable NUV1 message without changing metadata."""
        return self._wire_bytes

    @property
    def device_uuid(self) -> str:
        """Device identity; empty for legacy recordings without UUID metadata."""
        return str(self.metadata.get("device_uuid", ""))

    @property
    def run_uuid(self) -> str:
        """Daemon run identity; empty for legacy recordings without UUID metadata."""
        return str(self.metadata.get("run_uuid", ""))

    @property
    def identity(self) -> tuple[str, str, int, int]:
        """Capture identity, including the daemon run across counter resets."""
        return self.device_uuid, self.run_uuid, self.capture_epoch, self.capture_sequence


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
    if (not isinstance(metadata, dict) or metadata.get("schema_version") != 1
            or not isinstance(metadata.get("products"), list)):
        raise ValueError("unsupported Nuvo frame metadata schema")
    for name in ("device_uuid", "run_uuid"):
        value = metadata.get(name)
        if name in metadata and (not isinstance(value, str) or len(value) != 32
                                  or any(character not in "0123456789abcdef" for character in value)):
            raise ValueError("invalid Nuvo frame UUID metadata")
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
            or name not in PRODUCT_FORMATS
            or name in products
            or not isinstance(data_format, str)
            or data_format not in PRODUCT_FORMATS.get(name, set())
            or offset < 0
            or length <= 0
            or width <= 0
            or height <= 0
            or offset + length > payload_size
        ):
            raise ValueError("invalid or duplicate Nuvo product descriptor")
        element_size = 4 if data_format == "float32_le" else 3 if data_format in {"bgr8", "rgb8"} else 1
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
    if not products:
        raise ValueError("Nuvo frame has no selected products")
    mask = sum(1 << tuple(PRODUCT_FORMATS).index(name) for name in products)
    for name in ("available_products", "selected_products"):
        if name in metadata:
            value = metadata[name]
            if (not isinstance(value, int) or isinstance(value, bool) or
                    not 0 < value < (1 << len(PRODUCT_FORMATS)) or
                    (name == "selected_products" and value != mask) or
                    (name == "available_products" and mask & ~value)):
                raise ValueError("Nuvo frame product mask disagrees with its descriptors")
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
