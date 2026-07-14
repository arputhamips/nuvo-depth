#!/usr/bin/env python3
"""Regression tests for OpenCV map formats accepted by the EGL adapter."""

from __future__ import annotations

import sys
import types
from pathlib import Path

import cv2
import numpy as np


class _ColorSpace:
    @staticmethod
    def Sycc() -> object:
        return object()


class _Transform:
    def __init__(self, **_: object) -> None:
        pass


sys.modules.setdefault(
    "libcamera",
    types.SimpleNamespace(ColorSpace=_ColorSpace, Transform=_Transform),
)
sys.modules.setdefault(
    "picamera2",
    types.SimpleNamespace(MappedArray=object, Picamera2=object),
)
sys.path.insert(0, str(Path(__file__).parents[1] / "tools" / "pi"))

from depthd_egl_gl_rectification import _float_maps  # noqa: E402


def test_fixed_point_maps_are_restored_to_float_coordinates() -> None:
    y, x = np.indices((7, 9), dtype=np.float32)
    x += 0.375
    y += 0.625
    fixed = cv2.convertMaps(x, y, cv2.CV_16SC2)

    restored_x, restored_y = _float_maps(fixed)

    assert restored_x.shape == x.shape
    assert restored_y.shape == y.shape
    assert restored_x.flags.c_contiguous
    assert restored_y.flags.c_contiguous
    np.testing.assert_allclose(restored_x, x, atol=1.0 / 32.0)
    np.testing.assert_allclose(restored_y, y, atol=1.0 / 32.0)


def test_float_maps_are_kept_as_contiguous_float32() -> None:
    y, x = np.indices((5, 8), dtype=np.float32)

    restored_x, restored_y = _float_maps((x[:, ::-1], y[:, ::-1]))

    assert restored_x.dtype == np.float32
    assert restored_y.dtype == np.float32
    assert restored_x.flags.c_contiguous
    assert restored_y.flags.c_contiguous


if __name__ == "__main__":
    test_fixed_point_maps_are_restored_to_float_coordinates()
    test_float_maps_are_kept_as_contiguous_float32()
