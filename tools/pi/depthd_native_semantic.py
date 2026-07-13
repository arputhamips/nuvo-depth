#!/usr/bin/env python3
"""Thin ctypes owner for the native latest-only Hailo semantic runtime."""

from __future__ import annotations

import ctypes
import threading
from pathlib import Path
from typing import Any

import numpy as np

IMAGE_WIDTH = 728
IMAGE_HEIGHT = 544

_ReleaseCallback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint64)


class _Config(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("semantic_ring_path", ctypes.c_char_p),
        ("target_frame", ctypes.c_char_p),
        ("yolo_hef_path", ctypes.c_char_p),
        ("segformer_hef_path", ctypes.c_char_p),
        ("yolo_hz", ctypes.c_double),
        ("segformer_hz", ctypes.c_double),
        ("preview_enabled", ctypes.c_uint32),
        ("preview_jpeg_quality", ctypes.c_uint32),
        ("release_frame", _ReleaseCallback),
        ("release_context", ctypes.c_void_p),
        ("optical_to_target", ctypes.c_double * 16),
    ]


class _Frame(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("lease_token", ctypes.c_uint64),
        ("source_sequence", ctypes.c_uint64),
        ("capture_realtime_ns", ctypes.c_int64),
        ("submitted_steady_ns", ctypes.c_int64),
        ("left_rectified_bgr", ctypes.POINTER(ctypes.c_uint8)),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("color_stride_bytes", ctypes.c_uint32),
        ("disparity", ctypes.POINTER(ctypes.c_float)),
        ("disparity_stride_elements", ctypes.c_uint32),
        ("valid_x_offset", ctypes.c_uint32),
        ("valid_y_offset", ctypes.c_uint32),
        ("valid_width", ctypes.c_uint32),
        ("valid_height", ctypes.c_uint32),
        ("min_disparity", ctypes.c_float),
        ("max_disparity", ctypes.c_float),
    ]


class _ModelStats(ctypes.Structure):
    _fields_ = [
        ("model_name", ctypes.c_char * 48),
        ("completed_runs", ctypes.c_uint64),
        ("errors", ctypes.c_uint64),
        ("last_latency_ms", ctypes.c_double),
        ("max_latency_ms", ctypes.c_double),
    ]


class _Stats(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("submitted_frames", ctypes.c_uint64),
        ("replaced_pending_frames", ctypes.c_uint64),
        ("processed_frames", ctypes.c_uint64),
        ("published_results", ctypes.c_uint64),
        ("worker_errors", ctypes.c_uint64),
        ("last_frame_latency_ms", ctypes.c_double),
        ("max_frame_latency_ms", ctypes.c_double),
        ("pending", ctypes.c_uint32),
        ("model_count", ctypes.c_uint32),
        ("models", _ModelStats * 8),
    ]


def _require_array(
    value: np.ndarray,
    *,
    dtype: np.dtype,
    shape: tuple[int, ...],
    name: str,
) -> np.ndarray:
    if not isinstance(value, np.ndarray):
        raise TypeError(f"{name} must be a numpy array")
    if value.dtype != dtype:
        raise TypeError(f"{name} must have dtype {dtype}")
    if value.shape != shape:
        raise ValueError(f"{name} must have shape {shape}")
    if not value.flags.c_contiguous:
        raise ValueError(f"{name} must be C-contiguous")
    return value


class NativeSemanticWorker:
    """Owns the C++ worker while Python only retains submitted array leases."""

    def __init__(
        self,
        *,
        semantic_ring: Path,
        yolo_hef: Path,
        segformer_hef: Path,
        yolo_hz: float = 5.0,
        segformer_hz: float = 5.0,
        preview_enabled: bool = False,
        runtime_library: Path | None = None,
        target_frame: str = "base_link",
    ) -> None:
        if runtime_library is None:
            runtime_library = (
                Path(__file__).resolve().parents[2]
                / "build"
                / "libdepthd_native_semantic.so"
            )
        self._library = ctypes.CDLL(str(runtime_library))
        self._bind_library()
        if self._library.depthd_native_semantic_hailort_available() != 1:
            raise RuntimeError(
                "native semantic library was built without HailoRT support"
            )
        self._lease_lock = threading.Lock()
        self._leases: dict[int, tuple[np.ndarray, np.ndarray]] = {}
        self._next_token = 1
        self._released_leases = 0
        self._final_stats: dict[str, Any] | None = None
        self._release_callback = _ReleaseCallback(self._release_lease)
        self._handle: int | None = None

        config = _Config()
        self._library.depthd_native_semantic_default_config(ctypes.byref(config))
        strings = {
            "ring": str(semantic_ring).encode(),
            "target": target_frame.encode(),
            "yolo": str(yolo_hef).encode(),
            "segformer": str(segformer_hef).encode(),
        }
        self._config_strings = strings
        config.semantic_ring_path = strings["ring"]
        config.target_frame = strings["target"]
        config.yolo_hef_path = strings["yolo"]
        config.segformer_hef_path = strings["segformer"]
        config.yolo_hz = yolo_hz
        config.segformer_hz = segformer_hz
        config.preview_enabled = int(preview_enabled)
        config.release_frame = self._release_callback
        error = ctypes.create_string_buffer(512)
        self._handle = self._library.depthd_native_semantic_create(
            ctypes.byref(config), error, len(error)
        )
        if not self._handle:
            raise RuntimeError(error.value.decode(errors="replace"))

    def _bind_library(self) -> None:
        library = self._library
        library.depthd_native_semantic_default_config.argtypes = [
            ctypes.POINTER(_Config)
        ]
        library.depthd_native_semantic_default_config.restype = None
        library.depthd_native_semantic_hailort_available.argtypes = []
        library.depthd_native_semantic_hailort_available.restype = ctypes.c_int
        library.depthd_native_semantic_create.argtypes = [
            ctypes.POINTER(_Config),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        library.depthd_native_semantic_create.restype = ctypes.c_void_p
        library.depthd_native_semantic_wants_frame.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int64,
        ]
        library.depthd_native_semantic_wants_frame.restype = ctypes.c_int
        library.depthd_native_semantic_submit.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_Frame),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        library.depthd_native_semantic_submit.restype = ctypes.c_int
        library.depthd_native_semantic_get_stats.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_Stats),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        library.depthd_native_semantic_get_stats.restype = ctypes.c_int
        library.depthd_native_semantic_wait_for_published.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_uint32,
        ]
        library.depthd_native_semantic_wait_for_published.restype = ctypes.c_int
        library.depthd_native_semantic_destroy.argtypes = [ctypes.c_void_p]
        library.depthd_native_semantic_destroy.restype = None

    def _release_lease(self, _context: int, token: int) -> None:
        with self._lease_lock:
            if self._leases.pop(int(token), None) is not None:
                self._released_leases += 1

    def wants_frame(self, now_ns: int) -> bool:
        handle = self._handle
        return bool(
            handle
            and self._library.depthd_native_semantic_wants_frame(handle, now_ns)
        )

    def submit(
        self,
        *,
        source_sequence: int,
        capture_realtime_ns: int,
        color_bgr: np.ndarray,
        disparity: np.ndarray,
        submitted_ns: int,
    ) -> bool:
        handle = self._handle
        if not handle:
            return False
        color_bgr = _require_array(
            color_bgr,
            dtype=np.dtype(np.uint8),
            shape=(IMAGE_HEIGHT, IMAGE_WIDTH, 3),
            name="left_rectified_color",
        )
        disparity = _require_array(
            disparity,
            dtype=np.dtype(np.float32),
            shape=(IMAGE_HEIGHT, IMAGE_WIDTH),
            name="disparity",
        )
        with self._lease_lock:
            token = self._next_token
            self._next_token += 1
            self._leases[token] = (color_bgr, disparity)
        frame = _Frame()
        frame.struct_size = ctypes.sizeof(frame)
        frame.lease_token = token
        frame.source_sequence = source_sequence
        frame.capture_realtime_ns = capture_realtime_ns
        frame.submitted_steady_ns = submitted_ns
        frame.left_rectified_bgr = color_bgr.ctypes.data_as(
            ctypes.POINTER(ctypes.c_uint8)
        )
        frame.width = IMAGE_WIDTH
        frame.height = IMAGE_HEIGHT
        frame.color_stride_bytes = color_bgr.strides[0]
        frame.disparity = disparity.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        frame.disparity_stride_elements = disparity.strides[0] // 4
        frame.valid_x_offset = 128
        frame.valid_y_offset = 0
        frame.valid_width = IMAGE_WIDTH - 128
        frame.valid_height = IMAGE_HEIGHT
        frame.min_disparity = 0.0
        frame.max_disparity = 128.0
        error = ctypes.create_string_buffer(512)
        accepted = self._library.depthd_native_semantic_submit(
            handle, ctypes.byref(frame), error, len(error)
        )
        if accepted != 1:
            # Native submission releases rejected leases. This pop is only a
            # defensive fallback for an ABI/runtime failure before ownership.
            with self._lease_lock:
                self._leases.pop(token, None)
            message = error.value.decode(errors="replace")
            if message:
                raise RuntimeError(message)
            return False
        return True

    def wait_for_published(self, count: int, timeout_s: float) -> bool:
        handle = self._handle
        return bool(
            handle
            and self._library.depthd_native_semantic_wait_for_published(
                handle, count, max(0, round(timeout_s * 1000.0))
            )
        )

    def stats(self) -> dict[str, Any]:
        handle = self._handle
        if not handle:
            return self._final_stats or {
                "closed": True,
                "outstanding_leases": 0,
            }
        native = _Stats()
        native.struct_size = ctypes.sizeof(native)
        error = ctypes.create_string_buffer(512)
        if (
            self._library.depthd_native_semantic_get_stats(
                handle, ctypes.byref(native), error, len(error)
            )
            != 0
        ):
            raise RuntimeError(error.value.decode(errors="replace"))
        with self._lease_lock:
            outstanding = len(self._leases)
            released = self._released_leases
        models = {}
        for index in range(native.model_count):
            model = native.models[index]
            name = bytes(model.model_name).split(b"\0", 1)[0].decode(
                errors="replace"
            )
            models[name] = {
                "completed_runs": int(model.completed_runs),
                "errors": int(model.errors),
                "last_latency_ms": float(model.last_latency_ms),
                "max_latency_ms": float(model.max_latency_ms),
            }
        return {
            "submitted_frames": int(native.submitted_frames),
            "replaced_pending_frames": int(native.replaced_pending_frames),
            "processed_frames": int(native.processed_frames),
            "published_results": int(native.published_results),
            "worker_errors": int(native.worker_errors),
            "last_frame_latency_ms": float(native.last_frame_latency_ms),
            "max_frame_latency_ms": float(native.max_frame_latency_ms),
            "pending": bool(native.pending),
            "outstanding_leases": outstanding,
            "released_leases": released,
            "models": models,
        }

    def close(self) -> None:
        handle = self._handle
        if not handle:
            return
        before = self.stats()
        expected = int(before["submitted_frames"]) - int(
            before["replaced_pending_frames"]
        )
        self._library.depthd_native_semantic_wait_for_published(
            handle, expected, 10_000
        )
        final = self.stats()
        self._handle = None
        self._library.depthd_native_semantic_destroy(handle)
        with self._lease_lock:
            if self._leases:
                raise RuntimeError(
                    f"native semantic runtime leaked {len(self._leases)} frame leases"
                )
        final["closed"] = True
        final["pending"] = False
        final["outstanding_leases"] = 0
        self._final_stats = final

    def __enter__(self) -> "NativeSemanticWorker":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()
