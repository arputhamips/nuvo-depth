#!/usr/bin/env python3
"""Thin ctypes binding for the native compact-cloud/result-ring publisher."""

from __future__ import annotations

import ctypes
import time
from pathlib import Path

import numpy as np


class _Config(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("result_ring_path", ctypes.c_char_p),
        ("optical_to_base", ctypes.c_double * 16),
    ]


class _Frame(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("capture_realtime_ns", ctypes.c_int64),
        ("health", ctypes.c_uint32),
        ("disparity", ctypes.POINTER(ctypes.c_float)),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("disparity_stride_elements", ctypes.c_uint32),
        ("left_rectified_mono", ctypes.POINTER(ctypes.c_uint8)),
        ("left_rectified_mono_bytes", ctypes.c_uint32),
        ("left_rectified_jpeg", ctypes.POINTER(ctypes.c_uint8)),
        ("left_rectified_jpeg_bytes", ctypes.c_uint32),
    ]


class _Stats(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("obstacle_point_count", ctypes.c_uint32),
        ("obstacle_cloud_ms", ctypes.c_double),
        ("result_ring_publish_ms", ctypes.c_double),
        ("total_ms", ctypes.c_double),
        ("sampled_pixels", ctypes.c_uint64),
        ("accepted_raw_points", ctypes.c_uint64),
        ("accumulated_voxels", ctypes.c_uint64),
        ("supported_voxels", ctypes.c_uint64),
        ("accepted_clusters", ctypes.c_uint64),
        ("voxel_capacity_overflow", ctypes.c_uint64),
    ]


def sensor_monotonic_to_realtime_ns(sensor_timestamp_ns: int) -> int:
    """Translate libcamera's monotonic SensorTimestamp to Unix wall time."""
    if sensor_timestamp_ns <= 0:
        raise ValueError("libcamera SensorTimestamp must be positive")
    return sensor_timestamp_ns + time.time_ns() - time.monotonic_ns()


def _require_array(
    value: np.ndarray,
    *,
    dtype: np.dtype,
    shape: tuple[int, ...] | None,
    name: str,
) -> np.ndarray:
    if not isinstance(value, np.ndarray):
        raise TypeError(f"{name} must be a numpy array")
    if value.dtype != dtype:
        raise TypeError(f"{name} must have dtype {dtype}")
    if shape is not None and value.shape != shape:
        raise ValueError(f"{name} must have shape {shape}")
    if not value.flags.c_contiguous:
        raise ValueError(f"{name} must be C-contiguous")
    return value


class NativeOutputPublisher:
    """Own one native writer; exactly one instance may own a ring path."""

    def __init__(
        self,
        result_ring_path: Path,
        library_path: Path | None = None,
    ) -> None:
        self._handle = None
        if library_path is None:
            library_path = (
                Path(__file__).resolve().parents[2]
                / "build"
                / "libdepthd_native_output.so"
            )
        self._library = ctypes.CDLL(str(library_path))
        self._library.depthd_native_output_default_config.argtypes = [
            ctypes.POINTER(_Config)
        ]
        self._library.depthd_native_output_default_config.restype = None
        self._library.depthd_native_output_create.argtypes = [
            ctypes.POINTER(_Config),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self._library.depthd_native_output_create.restype = ctypes.c_void_p
        self._library.depthd_native_output_publish.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_Frame),
            ctypes.POINTER(_Stats),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self._library.depthd_native_output_publish.restype = ctypes.c_int
        self._library.depthd_native_output_destroy.argtypes = [ctypes.c_void_p]
        self._library.depthd_native_output_destroy.restype = None

        config = _Config()
        self._library.depthd_native_output_default_config(ctypes.byref(config))
        self._ring_path_bytes = str(result_ring_path).encode("utf-8")
        config.result_ring_path = self._ring_path_bytes
        error = ctypes.create_string_buffer(512)
        self._handle = self._library.depthd_native_output_create(
            ctypes.byref(config), error, len(error)
        )
        if not self._handle:
            raise RuntimeError(error.value.decode("utf-8", errors="replace"))

    def publish(
        self,
        *,
        capture_realtime_ns: int,
        disparity: np.ndarray,
        left_rectified_mono: np.ndarray | None = None,
        left_rectified_jpeg: np.ndarray | None = None,
    ) -> dict[str, float | int]:
        if not self._handle:
            raise RuntimeError("native output publisher is closed")
        disparity = _require_array(
            disparity,
            dtype=np.dtype(np.float32),
            shape=(544, 728),
            name="disparity",
        )
        mono = None
        if left_rectified_mono is not None:
            mono = _require_array(
                left_rectified_mono,
                dtype=np.dtype(np.uint8),
                shape=(544, 728),
                name="left_rectified_mono",
            )
        jpeg = None
        if left_rectified_jpeg is not None:
            jpeg = _require_array(
                left_rectified_jpeg,
                dtype=np.dtype(np.uint8),
                shape=None,
                name="left_rectified_jpeg",
            )

        null_u8 = ctypes.POINTER(ctypes.c_uint8)()
        frame = _Frame()
        frame.struct_size = ctypes.sizeof(frame)
        frame.capture_realtime_ns = capture_realtime_ns
        frame.health = 1
        frame.disparity = disparity.ctypes.data_as(
            ctypes.POINTER(ctypes.c_float)
        )
        frame.width = disparity.shape[1]
        frame.height = disparity.shape[0]
        frame.disparity_stride_elements = disparity.strides[0] // 4
        frame.left_rectified_mono = (
            mono.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))
            if mono is not None
            else null_u8
        )
        frame.left_rectified_mono_bytes = 0 if mono is None else mono.nbytes
        frame.left_rectified_jpeg = (
            jpeg.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))
            if jpeg is not None
            else null_u8
        )
        frame.left_rectified_jpeg_bytes = 0 if jpeg is None else jpeg.nbytes
        stats = _Stats()
        stats.struct_size = ctypes.sizeof(stats)
        error = ctypes.create_string_buffer(512)
        code = self._library.depthd_native_output_publish(
            self._handle,
            ctypes.byref(frame),
            ctypes.byref(stats),
            error,
            len(error),
        )
        if code != 0:
            raise RuntimeError(error.value.decode("utf-8", errors="replace"))
        return {
            "sequence": int(stats.sequence),
            "obstacle_point_count": int(stats.obstacle_point_count),
            "obstacle_cloud_ms": float(stats.obstacle_cloud_ms),
            "result_ring_publish_ms": float(stats.result_ring_publish_ms),
            "total_ms": float(stats.total_ms),
            "sampled_pixels": int(stats.sampled_pixels),
            "accepted_raw_points": int(stats.accepted_raw_points),
            "accumulated_voxels": int(stats.accumulated_voxels),
            "supported_voxels": int(stats.supported_voxels),
            "accepted_clusters": int(stats.accepted_clusters),
            "voxel_capacity_overflow": int(stats.voxel_capacity_overflow),
        }

    def close(self) -> None:
        if getattr(self, "_handle", None):
            self._library.depthd_native_output_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "NativeOutputPublisher":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()
