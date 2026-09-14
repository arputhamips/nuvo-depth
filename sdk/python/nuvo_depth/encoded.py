from __future__ import annotations

from dataclasses import dataclass
import json
import struct

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


def decode_encoded_frame(wire_bytes: bytes) -> EncodedFrame:
    if len(wire_bytes) < HEADER.size:
        raise ValueError("encoded frame is shorter than its header")
    magic,version,flags,metadata_bytes,payload_bytes,sequence,reserved = HEADER.unpack_from(wire_bytes)
    if magic != b"NVE1" or version != 1 or flags & ~1 or reserved != 0 or sequence == 0:
        raise ValueError("encoded frame header is invalid")
    if metadata_bytes == 0 or metadata_bytes > MAX_ENCODED_METADATA or payload_bytes == 0 or payload_bytes > MAX_ENCODED_PAYLOAD:
        raise ValueError("encoded frame bounds are invalid")
    if HEADER.size + metadata_bytes + payload_bytes != len(wire_bytes):
        raise ValueError("encoded frame length does not match its header")
    metadata=json.loads(wire_bytes[HEADER.size:HEADER.size+metadata_bytes])
    required={"schema_version","capture_epoch","capture_sequence","capture_time_ns","source","codec","mime_type","width","height","keyframe"}
    if metadata.get("schema_version") != 1 or not required.issubset(metadata):
        raise ValueError("encoded frame metadata is invalid")
    if bool(flags & 1) != bool(metadata["keyframe"]):
        raise ValueError("encoded keyframe metadata disagrees with its header")
    payload=memoryview(wire_bytes)[HEADER.size+metadata_bytes:]
    return EncodedFrame(sequence,bool(flags & 1),metadata,payload)
