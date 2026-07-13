#!/usr/bin/env python3

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np


sys.path.insert(0, str(Path(__file__).parents[1] / "tools" / "pi"))

from stereo_auto_exposure import (  # noqa: E402
    ABSOLUTE_MAX_SHUTTER_US,
    AutoExposureConfig,
    StereoAutoExposure,
)


def frame(value: int) -> np.ndarray:
    return np.full((272, 364), value, dtype=np.uint8)


class StereoAutoExposureTest(unittest.TestCase):
    def make_controller(
        self, shutter_us: int = 8_000, gain: float = 2.0
    ) -> StereoAutoExposure:
        return StereoAutoExposure(
            AutoExposureConfig(
                update_interval_frames=1,
                settle_frames_after_update=0,
            ),
            initial_shutter_us=shutter_us,
            initial_gain=gain,
        )

    def test_target_deadband_keeps_exposure(self) -> None:
        controller = self.make_controller()
        self.assertIsNone(controller.observe(frame(75), frame(75)))
        self.assertEqual(controller.shutter_us, 8_000)
        self.assertEqual(controller.gain, 2.0)

    def test_dark_scene_never_exceeds_limits(self) -> None:
        controller = self.make_controller()
        for _ in range(10):
            controller.observe(frame(10), frame(10))
        self.assertEqual(controller.shutter_us, ABSOLUTE_MAX_SHUTTER_US)
        self.assertEqual(controller.gain, 3.0)

    def test_bright_scene_reduces_gain_then_shutter(self) -> None:
        controller = self.make_controller()
        command = controller.observe(frame(200), frame(200))
        self.assertIsNotNone(command)
        assert command is not None
        self.assertLess(command.gain, 2.0)
        for _ in range(5):
            controller.observe(frame(200), frame(200))
        self.assertEqual(controller.gain, 1.0)
        self.assertLess(controller.shutter_us, 8_000)

    def test_one_clipped_camera_forces_fast_reduction(self) -> None:
        controller = self.make_controller()
        command = controller.observe(frame(75), frame(255))
        self.assertIsNotNone(command)
        assert command is not None
        self.assertEqual(command.reason, "clipping")
        self.assertLessEqual(command.shutter_us * command.gain, 8_000)

    def test_configuration_rejects_slower_than_one_over_125(self) -> None:
        with self.assertRaises(ValueError):
            AutoExposureConfig(max_shutter_us=8_001).validate()

    def test_configuration_rejects_gain_above_three(self) -> None:
        with self.assertRaises(ValueError):
            AutoExposureConfig(max_gain=3.01).validate()

    def test_diagnostic_update_history_is_bounded(self) -> None:
        controller = self.make_controller()
        for index in range(200):
            value = 10 if index % 2 == 0 else 220
            controller.observe(frame(value), frame(value))
        self.assertLessEqual(len(controller.report()["updates"]), 64)


if __name__ == "__main__":
    unittest.main()
