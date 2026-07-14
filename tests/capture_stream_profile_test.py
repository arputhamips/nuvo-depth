#!/usr/bin/env python3

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np


sys.path.insert(0, str(Path(__file__).parents[1] / "tools" / "pi"))

from capture_stream_profile import (  # noqa: E402
    ISP_LUMA_RGB_LEFT_PROFILE,
    RGB_GRAY_PROFILE,
    camera_stream_plan,
    visible_bgr,
    visible_y_plane,
)


class CaptureStreamProfileTest(unittest.TestCase):
    def test_legacy_profile_keeps_existing_rgb_contract(self) -> None:
        left = camera_stream_plan(RGB_GRAY_PROFILE, "left")
        right = camera_stream_plan(RGB_GRAY_PROFILE, "right")
        self.assertEqual(left.main_format, "RGB888")
        self.assertEqual(left.color_stream, "main")
        self.assertFalse(left.depth_is_gray)
        self.assertIsNone(right.color_stream)

    def test_isp_profile_uses_only_left_secondary_rgb(self) -> None:
        left = camera_stream_plan(ISP_LUMA_RGB_LEFT_PROFILE, "left")
        right = camera_stream_plan(ISP_LUMA_RGB_LEFT_PROFILE, "right")
        self.assertEqual(left.main_format, "YUV420")
        self.assertEqual(left.lores_format, "RGB888")
        self.assertEqual(left.color_stream, "lores")
        self.assertTrue(left.depth_is_gray)
        self.assertEqual(right.main_format, "YUV420")
        self.assertIsNone(right.lores_format)
        self.assertIsNone(right.color_stream)

    def test_visible_y_plane_is_zero_copy_and_preserves_padding(self) -> None:
        mapped = np.arange(816 * 768, dtype=np.uint8).reshape(816, 768)
        visible = visible_y_plane(mapped, width=728, height=544)
        self.assertEqual(visible.shape, (544, 728))
        self.assertEqual(visible.strides, mapped.strides)
        self.assertTrue(np.shares_memory(mapped, visible))

    def test_visible_bgr_is_zero_copy(self) -> None:
        mapped = np.zeros((544, 746, 3), dtype=np.uint8)
        visible = visible_bgr(mapped, width=728, height=544)
        self.assertEqual(visible.shape, (544, 728, 3))
        self.assertTrue(np.shares_memory(mapped, visible))

    def test_unknown_profile_and_side_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            camera_stream_plan("unknown", "left")
        with self.assertRaises(ValueError):
            camera_stream_plan(RGB_GRAY_PROFILE, "rear")


if __name__ == "__main__":
    unittest.main()
