"""Regression coverage for stereo sign errors outside the matcher search."""
from pathlib import Path
import sys
import unittest

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from stereo_validation_matrix import epipolar_metrics


class EpipolarDiagnosticsTest(unittest.TestCase):
    def test_geometry_is_measured_for_either_disparity_sign(self):
        rng = np.random.default_rng(812)
        left = cv2.GaussianBlur(rng.integers(0, 256, (360, 480), np.uint8), (3, 3), 0)
        for displacement in (-48, 48):
            with self.subTest(displacement=displacement):
                # right(x,y) = left(x-displacement,y-14): negative
                # disparity is intentional and must not disappear from scoring.
                right = cv2.warpAffine(left, np.float32([[1, 0, displacement], [0, 1, 14]]),
                                       (480, 360))
                metrics = epipolar_metrics(left, right)
                self.assertGreater(metrics["geometric_inlier_matches"], 100)
                self.assertAlmostEqual(metrics["horizontal_disparity_p50_px"], -displacement, delta=.2)
                self.assertAlmostEqual(metrics["vertical_abs_p50_px"], 14, delta=.2)
                if displacement > 0:
                    self.assertGreater(metrics["negative_disparity_fraction"], .95)
                else:
                    self.assertLess(metrics["negative_disparity_fraction"], .05)

    def test_blank_images_report_missing_texture(self):
        blank = np.zeros((360, 480), np.uint8)
        with self.assertRaisesRegex(RuntimeError, "not enough stereo texture"):
            epipolar_metrics(blank, blank)


if __name__ == "__main__":
    unittest.main()
