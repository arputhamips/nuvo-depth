from __future__ import annotations

from dataclasses import dataclass
import json
import struct

from .frame import _normalize_uuid

MAX_ENCODED_METADATA = 64 * 1024
MAX_ENCODED_PAYLOAD = 32 * 1024 * 1024
HEADER = struct.Struct(">4sHHIQQI")


@dataclass(frozen=True)
class EncodedFrame:
    stream_sequence: int
    keyframe: bool
    metadata: dict
    data: memoryview

    @property
    def codec(self) -> str:
        return str(self.metadata["codec"])

    @property
    def capture_sequence(self) -> int:
        return int(self.metadata["capture_sequence"])

    @property
    def capture_epoch(self) -> int:
        return int(self.metadata["capture_epoch"])

    @property
    def device_uuid(self) -> str:
        return _normalize_uuid(self.metadata["device_uuid"]) if "device_uuid" in self.metadata else ""

    @property
    def run_uuid(self) -> str:
        return _normalize_uuid(self.metadata["run_uuid"]) if "run_uuid" in self.metadata else ""

    @property
    def identity(self) -> tuple[str, str, int, int]:
        """Full capture identity; legacy envelopes may omit UUIDs."""
        return self.device_uuid, self.run_uuid, self.capture_epoch, self.capture_sequence


def decode_encoded_frame(wire_bytes: bytes) -> EncodedFrame:
    if not isinstance(wire_bytes, (bytes, bytearray, memoryview)):
        raise TypeError("encoded input must be bytes, bytearray, or memoryview")
    wire_size = wire_bytes.nbytes if isinstance(wire_bytes, memoryview) else len(wire_bytes)
    if not HEADER.size <= wire_size <= HEADER.size + MAX_ENCODED_METADATA + MAX_ENCODED_PAYLOAD:
        raise ValueError("encoded frame is outside its hard bounds")
    if not isinstance(wire_bytes, bytes):
        wire_bytes = bytes(wire_bytes)
    magic,version,flags,metadata_bytes,payload_bytes,sequence,reserved = HEADER.unpack_from(wire_bytes)
    if magic != b"NVE1" or version != 1 or flags & ~1 or reserved != 0 or sequence == 0:
        raise ValueError("encoded frame header is invalid")
    if metadata_bytes == 0 or metadata_bytes > MAX_ENCODED_METADATA or payload_bytes == 0 or payload_bytes > MAX_ENCODED_PAYLOAD:
        raise ValueError("encoded frame bounds are invalid")
    if HEADER.size + metadata_bytes + payload_bytes != len(wire_bytes):
        raise ValueError("encoded frame length does not match its header")
    metadata=json.loads(wire_bytes[HEADER.size:HEADER.size+metadata_bytes])
    required={"schema_version","capture_epoch","capture_sequence","capture_time_ns","source","codec","mime_type","width","height","keyframe"}
    if not isinstance(metadata, dict) or metadata.get("schema_version") != 1 or not required.issubset(metadata):
        raise ValueError("encoded frame metadata is invalid")
    for name in ("device_uuid", "run_uuid"):
        if name in metadata:
            _normalize_uuid(metadata[name])
    for name in ("capture_epoch", "capture_sequence", "capture_time_ns"):
        value = metadata[name]
        if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value < 1 << 64:
            raise ValueError("invalid encoded capture identity metadata")
    if not isinstance(metadata["keyframe"], bool) or bool(flags & 1) != metadata["keyframe"]:
        raise ValueError("encoded keyframe metadata disagrees with its header")
    if (metadata["codec"] not in {"jpeg", "h264"}
            or any(not isinstance(metadata[key], int) or isinstance(metadata[key], bool)
                   or not 0 < metadata[key] <= maximum
                   for key, maximum in (("width", 1920), ("height", 1080)))):
        raise ValueError("encoded frame dimensions or codec are invalid")
    payload=memoryview(wire_bytes)[HEADER.size+metadata_bytes:]
    return EncodedFrame(sequence,bool(flags & 1),metadata,payload)
