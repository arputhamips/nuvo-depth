#!/usr/bin/env python3
"""Exercise real C-ABI construction/teardown without fake DMABUF imports."""

from __future__ import annotations

import argparse
import sys
import tempfile
import types
from pathlib import Path

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

from async_stereo_capture import (  # noqa: E402
    DMABUF_MODIFIER_INVALID,
    DmabufPlaneMetadata,
)
from depthd_egl_gl_rectification import EglGlDmabufRectifier  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    args = parser.parse_args()
    y, x = np.indices((3, 4), dtype=np.float32)
    source = np.arange(12, dtype=np.uint8).reshape(3, 4)
    with tempfile.TemporaryFile() as left_file, tempfile.TemporaryFile() as right_file:
        left = DmabufPlaneMetadata(
            left_file.fileno(),
            1,
            10,
            4,
            3,
            4,
            0,
            DMABUF_MODIFIER_INVALID,
        )
        right = DmabufPlaneMetadata(
            right_file.fileno(),
            2,
            11,
            4,
            3,
            4,
            0,
            DMABUF_MODIFIER_INVALID,
        )
        rectifier = EglGlDmabufRectifier(
            (x, y),
            (x, y),
            library=args.library,
            cache_capacity=2,
            allow_cpu_fallback=True,
        )
        try:
            diagnostics = rectifier.diagnostics()
            assert diagnostics["selected_backend"] in {
                "egl_gl_dmabuf",
                "cpu_opencv",
            }
            assert diagnostics["cache_size"] == 0
            assert diagnostics["owned_fd_count"] == 0
            if diagnostics["selected_backend"] == "cpu_opencv":
                # An EGL-less runner takes the initialization-fallback path;
                # this is safe to exercise because no import is attempted.
                rectifier.rectify_pair(
                    left,
                    right,
                    cpu_left=source,
                    cpu_right=source,
                )
                np.testing.assert_array_equal(rectifier.left_output, source)
                np.testing.assert_array_equal(rectifier.right_output, source)
                assert diagnostics["fallback_reason"]
        finally:
            rectifier.close()


if __name__ == "__main__":
    main()
