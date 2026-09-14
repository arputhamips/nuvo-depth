from __future__ import annotations

from pathlib import Path
import struct
from typing import BinaryIO, Iterator

from .frame import FrameSet, MAX_METADATA, MAX_PAYLOAD, decode_frame

MAGIC = b"NVR1\x00\x00\x00\x01"
LENGTH = struct.Struct(">Q")
MAX_FRAME = 32 + MAX_METADATA + MAX_PAYLOAD


class RecordingWriter:
    def __init__(self, path: str | Path):
        self._file: BinaryIO = open(path, "xb")
        self._file.write(MAGIC)

    def write(self, frame: FrameSet) -> None:
        data = frame._wire_bytes
        if not 0 < len(data) <= MAX_FRAME:
            raise ValueError("recording frame exceeds bounds")
        self._file.write(LENGTH.pack(len(data)))
        self._file.write(data)
        self._file.flush()

    def close(self) -> None:
        self._file.close()

    def __enter__(self) -> "RecordingWriter": return self
    def __exit__(self, *_: object) -> None: self.close()


class RecordingReader:
    def __init__(self, path: str | Path):
        self._file: BinaryIO = open(path, "rb")
        if self._file.read(len(MAGIC)) != MAGIC:
            self._file.close()
            raise ValueError("not a Nuvo recording")

    def __iter__(self) -> Iterator[FrameSet]:
        while True:
            encoded = self._file.read(LENGTH.size)
            if not encoded: return
            if len(encoded) != LENGTH.size: raise ValueError("truncated record length")
            size = LENGTH.unpack(encoded)[0]
            if not 0 < size <= MAX_FRAME: raise ValueError("record exceeds bounds")
            data = self._file.read(size)
            if len(data) != size: raise ValueError("truncated recording frame")
            yield decode_frame(data)

    def close(self) -> None: self._file.close()
    def __enter__(self) -> "RecordingReader": return self
    def __exit__(self, *_: object) -> None: self.close()
