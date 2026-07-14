#!/usr/bin/env python3
"""Declarative Picamera2 stream layouts for native stereo capture."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


RGB_GRAY_PROFILE = "rgb_gray"
ISP_LUMA_RGB_LEFT_PROFILE = "isp_luma_rgb_left"
CAPTURE_STREAM_PROFILES = (
    RGB_GRAY_PROFILE,
    ISP_LUMA_RGB_LEFT_PROFILE,
)


@dataclass(frozen=True)
class CameraStreamPlan:
    """One camera's processed-output contract."""

    main_format: str
    lores_format: str | None
    depth_stream: str
    depth_is_gray: bool
    color_stream: str | None


def camera_stream_plan(profile: str, side: str) -> CameraStreamPlan:
    """Return a side-specific plan without importing Pi-only libraries."""
    if side not in {"left", "right"}:
        raise ValueError(f"unknown stereo side: {side}")
    if profile == RGB_GRAY_PROFILE:
        return CameraStreamPlan(
            main_format="RGB888",
            lores_format=None,
            depth_stream="main",
            depth_is_gray=False,
            color_stream="main" if side == "left" else None,
        )
    if profile == ISP_LUMA_RGB_LEFT_PROFILE:
        return CameraStreamPlan(
            main_format="YUV420",
            lores_format="RGB888" if side == "left" else None,
            depth_stream="main",
            depth_is_gray=True,
            color_stream="lores" if side == "left" else None,
        )
    raise ValueError(f"unknown capture stream profile: {profile}")


def visible_y_plane(
    mapped_yuv: np.ndarray, *, width: int, height: int
) -> np.ndarray:
    """Return a zero-copy view of visible full-resolution YUV420 luma."""
    if (
        mapped_yuv.dtype != np.uint8
        or mapped_yuv.ndim != 2
        or mapped_yuv.shape[0] < height
        or mapped_yuv.shape[1] < width
    ):
        raise ValueError(
            f"invalid mapped YUV420 array {mapped_yuv.dtype} "
            f"{mapped_yuv.shape}; expected at least ({height}, {width})"
        )
    return mapped_yuv[:height, :width]


def visible_bgr(
    mapped_rgb: np.ndarray, *, width: int, height: int
) -> np.ndarray:
    """Return Picamera2 RGB888's visible BGR-ordered zero-copy view."""
    if (
        mapped_rgb.dtype != np.uint8
        or mapped_rgb.ndim != 3
        or mapped_rgb.shape[0] < height
        or mapped_rgb.shape[1] < width
        or mapped_rgb.shape[2] < 3
    ):
        raise ValueError(
            f"invalid mapped RGB888 array {mapped_rgb.dtype} "
            f"{mapped_rgb.shape}; expected at least ({height}, {width}, 3)"
        )
    return mapped_rgb[:height, :width, :3]
