"""Bounded NVP1 messages containing original capture-associated NUV1/NVE1 data."""

from __future__ import annotations

import struct
from typing import NamedTuple

from .frame import FrameSet, decode_frame
from .encoded import EncodedFrame, decode_encoded_frame


HEADER = struct.Struct(">4sHHQQQ")
MAX_PAIRED_RAW = 16 * 1024 * 1024
MAX_PAIRED_ENCODED = 4 * 1024 * 1024
MAX_PAIRED_WIRE = HEADER.size + MAX_PAIRED_RAW + MAX_PAIRED_ENCODED


class PairedFrame(NamedTuple):
    """Numeric/uncompressed products and preview from exactly one capture."""

    frame: FrameSet
    encoded: EncodedFrame

    @property
    def identity(self) -> tuple[str, str, int, int]:
        return self.frame.identity


def _identity(frame: FrameSet | EncodedFrame) -> tuple[str, str, int, int]:
    identity = frame.identity
    if any(not value or value == "0" * 32 for value in identity[:2]):
        raise ValueError("paired frames require complete device and run identities")
    for name in ("capture_epoch", "capture_sequence"):
        value = frame.metadata.get(name)
        if (not isinstance(value, int) or isinstance(value, bool)
                or not 0 < value < 1 << 64):
            raise ValueError("paired frames require integer capture identities")
    return identity


def decode_paired_frame(wire_bytes: bytes) -> PairedFrame:
    if not isinstance(wire_bytes, (bytes, bytearray, memoryview)):
        raise TypeError("paired input must be bytes, bytearray, or memoryview")
    size = wire_bytes.nbytes if isinstance(wire_bytes, memoryview) else len(wire_bytes)
    if not HEADER.size <= size <= MAX_PAIRED_WIRE:
        raise ValueError("paired message is outside its hard bounds")
    if not isinstance(wire_bytes, bytes):
        wire_bytes = bytes(wire_bytes)
    magic, version, flags, raw_size, encoded_size, reserved = HEADER.unpack_from(wire_bytes)
    if magic != b"NVP1" or version != 1 or flags or reserved:
        raise ValueError("invalid paired message header")
    if (not 32 < raw_size <= MAX_PAIRED_RAW or not 32 < encoded_size <= MAX_PAIRED_ENCODED
            or HEADER.size + raw_size + encoded_size != size):
        raise ValueError("invalid paired message lengths")
    raw_end = HEADER.size + raw_size
    raw = decode_frame(wire_bytes[HEADER.size:raw_end])
    encoded = decode_encoded_frame(wire_bytes[raw_end:])
    if _identity(raw) != _identity(encoded):
        raise ValueError("paired message contains different capture identities")
    return PairedFrame(raw, encoded)
