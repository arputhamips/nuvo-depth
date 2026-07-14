#!/usr/bin/env python3
"""Narrow ctypes bridge for capture-native EGL/OpenGL rectification."""

from __future__ import annotations

import ctypes
from pathlib import Path
from typing import Any

import numpy as np

from async_stereo_capture import DmabufPlaneMetadata


class _Plane(ctypes.Structure):
    _fields_ = [
        ("fd", ctypes.c_int),
        ("buffer_id", ctypes.c_uint64),
        ("request_id", ctypes.c_uint64),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("stride_bytes", ctypes.c_uint32),
        ("offset_bytes", ctypes.c_uint32),
        ("modifier", ctypes.c_uint64),
    ]


class _Timing(ctypes.Structure):
    _fields_ = [
        ("input_transfer_ms", ctypes.c_double),
        ("compute_ms", ctypes.c_double),
        ("output_transfer_ms", ctypes.c_double),
        ("total_ms", ctypes.c_double),
        ("submit_wait_ms", ctypes.c_double),
    ]


class _Diagnostics(ctypes.Structure):
    _fields_ = [
        ("cache_size", ctypes.c_uint64),
        ("cache_capacity", ctypes.c_uint64),
        ("import_count", ctypes.c_uint64),
        ("stale_reimport_count", ctypes.c_uint64),
        ("eviction_count", ctypes.c_uint64),
        ("failure_count", ctypes.c_uint64),
        ("owned_fd_count", ctypes.c_uint64),
        ("gpu_timer_supported", ctypes.c_int),
        ("gpu_active", ctypes.c_int),
    ]


def _u8_pointer(array: np.ndarray | None) -> ctypes.POINTER(ctypes.c_uint8):
    if array is None:
        return ctypes.POINTER(ctypes.c_uint8)()
    return array.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))


def _plane(metadata: DmabufPlaneMetadata) -> _Plane:
    return _Plane(
        metadata.fd,
        metadata.buffer_id,
        metadata.request_id,
        metadata.width,
        metadata.height,
        metadata.stride_bytes,
        metadata.offset_bytes,
        metadata.modifier,
    )


class EglGlDmabufRectifier:
    """Own the native backend and fixed caller-visible output arrays."""

    def __init__(
        self,
        left_maps: tuple[np.ndarray, np.ndarray],
        right_maps: tuple[np.ndarray, np.ndarray],
        *,
        library: Path,
        cache_capacity: int = 8,
        allow_cpu_fallback: bool = True,
    ) -> None:
        self._library = ctypes.CDLL(str(library))
        self._configure_abi()
        left_x, left_y = (
            np.ascontiguousarray(value, dtype=np.float32) for value in left_maps
        )
        right_x, right_y = (
            np.ascontiguousarray(value, dtype=np.float32) for value in right_maps
        )
        if left_x.shape != left_y.shape or left_x.shape != right_x.shape or (
            left_x.shape != right_y.shape
        ):
            raise ValueError("left/right rectification maps must have equal shapes")
        self.height, self.width = left_x.shape
        float_pointer = ctypes.POINTER(ctypes.c_float)
        self._handle = self._library.depthd_egl_gl_rectifier_create(
            self.width,
            self.height,
            left_x.ctypes.data_as(float_pointer),
            left_y.ctypes.data_as(float_pointer),
            right_x.ctypes.data_as(float_pointer),
            right_y.ctypes.data_as(float_pointer),
            cache_capacity,
            int(allow_cpu_fallback),
        )
        if not self._handle:
            raise RuntimeError(self._last_error())
        self.left_output = np.empty((self.height, self.width), dtype=np.uint8)
        self.right_output = np.empty((self.height, self.width), dtype=np.uint8)

    def _configure_abi(self) -> None:
        library = self._library
        float_pointer = ctypes.POINTER(ctypes.c_float)
        library.depthd_egl_gl_rectifier_create.argtypes = [
            ctypes.c_uint32,
            ctypes.c_uint32,
            float_pointer,
            float_pointer,
            float_pointer,
            float_pointer,
            ctypes.c_size_t,
            ctypes.c_int,
        ]
        library.depthd_egl_gl_rectifier_create.restype = ctypes.c_void_p
        library.depthd_egl_gl_rectifier_rectify_pair.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_Plane),
            ctypes.POINTER(_Plane),
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_size_t,
            ctypes.POINTER(_Timing),
        ]
        library.depthd_egl_gl_rectifier_rectify_pair.restype = ctypes.c_int
        library.depthd_egl_gl_rectifier_diagnostics.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_Diagnostics),
        ]
        library.depthd_egl_gl_rectifier_diagnostics.restype = ctypes.c_int
        for name in (
            "depthd_egl_gl_rectifier_selected_backend",
            "depthd_egl_gl_rectifier_fallback_reason",
        ):
            getattr(library, name).argtypes = [ctypes.c_void_p]
            getattr(library, name).restype = ctypes.c_char_p
        library.depthd_egl_gl_rectifier_last_error.restype = ctypes.c_char_p
        library.depthd_egl_gl_rectifier_destroy.argtypes = [ctypes.c_void_p]

    def _last_error(self) -> str:
        error = self._library.depthd_egl_gl_rectifier_last_error()
        return error.decode("utf-8", errors="replace") if error else "unknown error"

    def rectify_pair(
        self,
        left: DmabufPlaneMetadata,
        right: DmabufPlaneMetadata,
        *,
        cpu_left: np.ndarray | None = None,
        cpu_right: np.ndarray | None = None,
    ) -> dict[str, float]:
        if (cpu_left is None) != (cpu_right is None):
            raise ValueError("CPU fallback requires both stereo views")
        if cpu_left is not None:
            for name, array in (("left", cpu_left), ("right", cpu_right)):
                if (
                    array.dtype != np.uint8
                    or array.ndim != 2
                    or array.shape[0] < self.height
                    or array.shape[1] < self.width
                ):
                    raise ValueError(f"invalid {name} CPU fallback view")
        left_plane = _plane(left)
        right_plane = _plane(right)
        timing = _Timing()
        result = self._library.depthd_egl_gl_rectifier_rectify_pair(
            self._handle,
            ctypes.byref(left_plane),
            ctypes.byref(right_plane),
            _u8_pointer(cpu_left),
            0 if cpu_left is None else cpu_left.strides[0],
            _u8_pointer(cpu_right),
            0 if cpu_right is None else cpu_right.strides[0],
            _u8_pointer(self.left_output),
            self.left_output.strides[0],
            _u8_pointer(self.right_output),
            self.right_output.strides[0],
            ctypes.byref(timing),
        )
        if result != 0:
            raise RuntimeError(self._last_error())
        return {name: float(getattr(timing, name)) for name, _ in timing._fields_}

    def diagnostics(self) -> dict[str, Any]:
        diagnostics = _Diagnostics()
        if self._library.depthd_egl_gl_rectifier_diagnostics(
            self._handle, ctypes.byref(diagnostics)
        ) != 0:
            raise RuntimeError(self._last_error())
        selected = self._library.depthd_egl_gl_rectifier_selected_backend(
            self._handle
        )
        reason = self._library.depthd_egl_gl_rectifier_fallback_reason(
            self._handle
        )
        report: dict[str, Any] = {
            name: int(getattr(diagnostics, name))
            for name, _ in diagnostics._fields_
        }
        report["selected_backend"] = selected.decode() if selected else ""
        report["fallback_reason"] = reason.decode() if reason else ""
        return report

    def close(self) -> None:
        if self._handle:
            self._library.depthd_egl_gl_rectifier_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> EglGlDmabufRectifier:
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()
