#!/usr/bin/env python3
"""Exercise the Picamera2-request to native DMABUF metadata seam."""

from __future__ import annotations

import os
import sys
import tempfile
import types
import unittest
from pathlib import Path


class _ColorSpace:
    @staticmethod
    def Sycc() -> object:
        return object()


class _Transform:
    def __init__(self, **_: object) -> None:
        pass


class _MappedArray:
    pass


sys.modules.setdefault(
    "libcamera",
    types.SimpleNamespace(ColorSpace=_ColorSpace, Transform=_Transform),
)
sys.modules.setdefault(
    "picamera2",
    types.SimpleNamespace(MappedArray=_MappedArray, Picamera2=object),
)
sys.path.insert(0, str(Path(__file__).parents[1] / "tools" / "pi"))

from async_stereo_capture import (  # noqa: E402
    DMABUF_MODIFIER_INVALID,
    FrameLease,
    StereoPairLease,
)
from capture_stream_profile import ISP_LUMA_RGB_LEFT_PROFILE  # noqa: E402


class _Plane:
    def __init__(self, fd: int, offset: int) -> None:
        self.fd = fd
        self.offset = offset


class _Framebuffer:
    def __init__(self, fd: int, offset: int) -> None:
        self.planes = [_Plane(fd, offset)]


class _Picamera:
    def __init__(self, stream: object) -> None:
        self.stream_map = {"main": stream}

    @staticmethod
    def camera_configuration() -> dict[str, dict[str, int]]:
        return {"main": {"stride": 768}}


class _NativeRequest:
    def __init__(self, stream: object, framebuffer: _Framebuffer) -> None:
        self.buffers = {stream: framebuffer}


class _Completed:
    def __init__(self, fd: int) -> None:
        stream = object()
        self.picam2 = _Picamera(stream)
        self.request = _NativeRequest(stream, _Framebuffer(fd, 32))
        self.releases = 0

    def release(self) -> None:
        self.releases += 1


def _frame(side: str, fd: int, timestamp: int) -> FrameLease:
    frame = object.__new__(FrameLease)
    frame.side = side
    frame.request = _Completed(fd)
    frame.metadata = {"SensorTimestamp": timestamp}
    frame.callback_monotonic_ns = timestamp + 100
    frame.sensor_timestamp_ns = timestamp
    frame._closed = False
    return frame


class DmabufCaptureAdapterTest(unittest.TestCase):
    def test_metadata_is_extracted_without_mapping(self) -> None:
        with (
            tempfile.TemporaryFile() as left_file,
            tempfile.TemporaryFile() as right_file,
        ):
            left = _frame("left", left_file.fileno(), 10_000)
            right = _frame("right", right_file.fileno(), 10_010)
            pair = StereoPairLease(left, right)
            views = pair.dmabuf_luma_views(
                stream_profile=ISP_LUMA_RGB_LEFT_PROFILE
            )
            self.assertEqual(views.left.fd, left_file.fileno())
            self.assertEqual(views.right.fd, right_file.fileno())
            self.assertEqual(views.left.stride_bytes, 768)
            self.assertEqual(views.left.offset_bytes, 32)
            self.assertEqual(views.left.width, 728)
            self.assertEqual(views.left.height, 544)
            self.assertEqual(views.left.modifier, DMABUF_MODIFIER_INVALID)
            self.assertEqual(views.left.request_id, 10_000)
            self.assertNotEqual(views.left.buffer_id, views.right.buffer_id)
            expected = os.fstat(left_file.fileno())
            expected_id = (
                ((expected.st_dev & 0xFFFFFFFF) << 32) ^ expected.st_ino
            ) & ((1 << 64) - 1)
            self.assertEqual(views.left.buffer_id, expected_id)
            pair.close()
            self.assertEqual(left.request.releases, 1)
            with self.assertRaisesRegex(RuntimeError, "already closed"):
                pair.dmabuf_luma_views(
                    stream_profile=ISP_LUMA_RGB_LEFT_PROFILE
                )


if __name__ == "__main__":
    unittest.main()
