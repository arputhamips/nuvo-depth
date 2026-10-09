#!/usr/bin/env python3
"""End-to-end synthetic ChArUco stereo calibration regression."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
from calibration_session_test import commit_fixture

try:
    import cv2
    import numpy as np
except ImportError:  # A minimal host build can omit the optional Python binding.
    cv2 = None
    np = None


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "nuvo_calibration", ROOT / "tools" / "calibration" / "nuvo_calibration.py")
CALIBRATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CALIBRATION)


class CharucoCompatibilityTest(unittest.TestCase):
    def test_missing_aruco_is_an_actionable_error(self) -> None:
        with patch.dict("sys.modules", {"cv2": SimpleNamespace(__version__="4.10.0")}):
            with self.assertRaisesRegex(RuntimeError, r"requires OpenCV >= 4\.6 with aruco support"):
                CALIBRATION._opencv_aruco()

    def test_pre_46_bindings_are_rejected(self) -> None:
        with patch.dict("sys.modules", {"cv2": SimpleNamespace(__version__="4.5.5")}):
            with self.assertRaisesRegex(RuntimeError, r"found 4\.5\.5"):
                CALIBRATION._opencv_aruco()

    def test_legacy_detector_interpolates_only_detected_markers(self) -> None:
        parameters = SimpleNamespace(cornerRefinementMethod=None)
        dictionary, board, image = object(), SimpleNamespace(dictionary=object()), object()
        board.dictionary = dictionary
        marker_corners, marker_ids = [object()], [[3]]
        detect = Mock(return_value=(marker_corners, marker_ids, []))
        interpolate = Mock(return_value=(2, "corners", "identifiers"))
        legacy = SimpleNamespace(DetectorParameters_create=lambda: parameters,
                                 CORNER_REFINE_NONE=0, detectMarkers=detect,
                                 interpolateCornersCharuco=interpolate)
        with patch.object(CALIBRATION, "_opencv_aruco", return_value=legacy):
            self.assertEqual(CALIBRATION._detect_charuco(image, board),
                             ("corners", "identifiers", marker_corners, marker_ids))
            detect.assert_called_once_with(image, dictionary, parameters=parameters)
            interpolate.assert_called_once_with(marker_corners, marker_ids, image, board, minMarkers=2)
            self.assertEqual(parameters.cornerRefinementMethod, 0)
            detect.return_value = ([], None, [])
            interpolate.reset_mock()
            self.assertEqual(CALIBRATION._detect_charuco(image, board), (None, None, [], None))
            interpolate.assert_not_called()

    def test_legacy_and_modern_target_rendering(self) -> None:
        for name in ("draw", "generateImage"):
            draw = Mock(return_value="image")
            board = SimpleNamespace(**{name: draw})
            self.assertEqual(CALIBRATION._draw_charuco_board(board, (700, 500)), "image")
            draw.assert_called_once_with((700, 500), marginSize=0, borderBits=1)


@unittest.skipIf(cv2 is None, "Python OpenCV is unavailable")
class SyntheticCharucoTest(unittest.TestCase):
    def test_odd_and_even_rows_keep_the_canonical_target_layout(self) -> None:
        for rows in (4, 5):
            target = {"type": "charuco", "squares_x": 7, "squares_y": rows,
                      "square_m": .03, "marker_m": .015, "dictionary": "DICT_5X5_100"}
            board = CALIBRATION._charuco_board({"target": target})
            pattern = CALIBRATION._draw_charuco_board(board, (700, rows * 100))
            self.assertEqual(int(pattern[0, 0]), 0)
            _, identifiers, _, _ = CALIBRATION._detect_charuco(pattern, board)
            self.assertIsNotNone(identifiers)
            np.testing.assert_array_equal(np.sort(identifiers.reshape(-1)), np.arange(6 * (rows - 1)))
        with self.assertRaisesRegex(ValueError, "unsupported ChArUco layout_version"):
            CALIBRATION._charuco_board({"target": {**target, "layout_version": "pre-4.6"}})

    def test_recovers_stereo_baseline_and_holdout_geometry(self) -> None:
        cv2.setNumThreads(1)
        configuration = {
            "schema_version": 1,
            "identity": {"unit_id": "synthetic-unit", "left_camera_id": "synthetic-left",
                         "right_camera_id": "synthetic-right"},
            "profile": {"preset_id": "synthetic-728x544", "sensor_mode": "synthetic-728x544",
                        "image_width": 728, "image_height": 544,
                        "frame_rates_hz": [60], "baseline_m": .1},
            "target": {"type": "charuco", "squares_x": 7, "squares_y": 5,
                       "square_m": .03, "marker_m": .015,
                       "dictionary": "DICT_5X5_100"},
            "thresholds": {"minimum_pairs": 12, "minimum_shared_corners": 6,
                           "maximum_rms_px": 1.5,
                           "maximum_epipolar_p95_px": 1.0,
                           "baseline_tolerance_m": .02, "minimum_coverage": .1},
        }
        with tempfile.TemporaryDirectory() as temporary:
            state_root = Path(temporary)
            status = CALIBRATION.create_web_session(state_root, configuration)
            session_id = status["session_id"]
            captures = state_root / "calibration-sessions" / session_id / "captures"
            board = CALIBRATION._charuco_board(configuration)
            pattern = CALIBRATION._draw_charuco_board(board, (700, 500))
            pattern = np.where(pattern < 128, 25, 235).astype(np.uint8)
            camera = np.asarray([[600., 0., 364.], [0., 600., 272.], [0., 0., 1.]])
            pattern_corners, pattern_ids, _, _ = CALIBRATION._detect_charuco(pattern, board)
            self.assertIsNotNone(pattern_ids)
            object_corners = CALIBRATION._charuco_board_corners(board)[pattern_ids.reshape(-1)]
            board_center = np.mean(object_corners, axis=0)
            poses = []
            for row, center_y in enumerate((175., 272., 369.)):
                for column, center_x in enumerate((300., 400., 500.)):
                    for distance_index, distance in enumerate((.44, .62, .84)):
                        poses.append((center_x, center_y, distance,
                                      .28 * (row - 1) + .035 * (distance_index - 1),
                                      -.30 * (column - 1) - .03 * (distance_index - 1),
                                      .10 * ((row + column + distance_index) % 3 - 1)))
            for index, (center_x, center_y, distance, rx, ry, rz) in enumerate(poses):
                rotation, _ = cv2.Rodrigues(np.asarray([rx, ry, rz]))
                desired_center = np.asarray(
                    [(center_x - 364.) * distance / 600.,
                     (center_y - 272.) * distance / 600., distance])
                translation = desired_center - rotation @ board_center
                images = []
                for camera_offset in (np.zeros(3), np.asarray([.1, 0., 0.])):
                    projected = []
                    for point in object_corners:
                        camera_point = rotation @ point + translation - camera_offset
                        pixel = camera @ camera_point
                        projected.append((pixel[:2] / pixel[2]).astype(np.float32))
                    homography, _ = cv2.findHomography(
                        pattern_corners.reshape(-1, 2), np.asarray(projected, np.float32))
                    images.append(cv2.warpPerspective(
                        pattern, homography, (728, 544),
                        flags=cv2.INTER_LINEAR, borderValue=205))
                capture_id = f"synthetic{index:03d}"
                cv2.imwrite(str(captures / f"{capture_id}-left.png"), images[0])
                cv2.imwrite(str(captures / f"{capture_id}-right.png"), images[1])
                commit_fixture(CALIBRATION, captures, session_id, capture_id,
                               {"left_camera_id": "synthetic-left", "right_camera_id": "synthetic-right"},
                               sequence=index + 1)
                status = CALIBRATION.analyze_web_capture(
                    state_root, session_id, capture_id, 0)
            self.assertGreaterEqual(status["accepted_pairs"], 12)
            self.assertGreaterEqual(status["coverage"]["score"], .1)
            solved = CALIBRATION.solve_web_session(state_root, session_id)
            validation = solved["validation"]
            self.assertEqual(validation["status"], "pass", validation)
            self.assertGreaterEqual(validation["holdout_pairs"], 2)
            self.assertLess(validation["rectified_vertical_error_p95_px"], 1.0)
            self.assertAlmostEqual(validation["baseline_m"], .1, delta=.02)
            CALIBRATION.validate_bundle(
                state_root / "calibration-sessions" / session_id / "bundle")
            saved_configuration = json.loads((captures.parent / "configuration.json").read_text())
            self.assertEqual(saved_configuration["target"]["layout_version"], "opencv-4.6")
            manifest = json.loads((captures.parent / "bundle/manifest.json").read_text())
            self.assertEqual(manifest["provenance"]["target"]["layout_version"], "opencv-4.6")


if __name__ == "__main__":
    unittest.main()
