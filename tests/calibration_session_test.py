#!/usr/bin/env python3
"""Session concurrency, resource admission and committed capture regressions."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("nuvo_calibration_sessions", ROOT / "tools/calibration/nuvo_calibration.py")
CALIBRATION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CALIBRATION)


def configuration() -> dict:
    return {
        "schema_version": 1,
        "identity": {"unit_id": "test-unit", "left_camera_id": "left-camera", "right_camera_id": "right-camera"},
        "profile": {"preset_id": "test-728x544", "sensor_mode": "test-mode", "image_width": 728,
                    "image_height": 544, "frame_rates_hz": [60], "baseline_m": .1},
        "target": {"type": "charuco", "squares_x": 7, "squares_y": 5, "square_m": .03,
                   "marker_m": .015, "dictionary": "DICT_5X5_100"},
        "thresholds": {"minimum_pairs": 12, "minimum_shared_corners": 6, "maximum_rms_px": 1.5,
                       "maximum_epipolar_p95_px": 1., "baseline_tolerance_m": .02, "minimum_coverage": .1},
    }


def commit_fixture(module, captures: Path, session_id: str, capture_id: str,
                   camera: dict, *, sequence: int = 1, frame: dict | None = None) -> dict:
    """Commit existing test PNGs through the production manifest contract."""
    frame = frame or {"device_uuid": "12345678-1234-5678-1234-567812345678",
                      "run_uuid": "87654321-4321-8765-4321-876543218765", "capture_epoch": 1,
                      "capture_sequence": sequence, "capture_timestamp_ns": 1_000_000 + sequence}
    metadata = {"schema_version": 1, "frame": frame, "sensor_delta_ns": 0, **camera}
    module.atomic_json(captures / f"{capture_id}.json", metadata)
    assets = {}
    config = json.loads((captures.parent / "configuration.json").read_text())
    for side, suffix in (("left", "-left.png"), ("right", "-right.png"), ("metadata", ".json")):
        path = captures / f"{capture_id}{suffix}"
        assets[side] = {"name": path.name, "bytes": path.stat().st_size,
                        "sha256": module.sha256(path), "frame": frame}
        if side != "metadata":
            assets[side].update(width=config["profile"]["image_width"], height=config["profile"]["image_height"])
    manifest = {"schema_version": 1, "state": "complete", "session_id": session_id,
                "capture_id": capture_id, "frame": frame, "assets": assets}
    module.atomic_json(captures / f"{capture_id}.complete.json", manifest)
    return manifest


class SessionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.config = configuration()
        self.status = CALIBRATION.create_web_session(self.root, self.config)
        self.session = self.status["session_id"]
        self.directory = self.root / "calibration-sessions" / self.session

    def fixture(self, capture_id: str) -> dict:
        import cv2
        import numpy as np
        captures = self.directory / "captures"
        for side in ("left", "right"):
            self.assertTrue(cv2.imwrite(str(captures / f"{capture_id}-{side}.png"), np.full((544, 728), 150, np.uint8)))
        return commit_fixture(CALIBRATION, captures, self.session, capture_id,
                              {"left_camera_id": "left-camera", "right_camera_id": "right-camera"})

    def capture_without_target(self, capture_id: str) -> dict:
        with patch.object(CALIBRATION, "_charuco_board", return_value=None), patch.object(
                CALIBRATION, "_detect_charuco", return_value=(None, None, None, None)):
            return CALIBRATION.analyze_web_capture(self.root, self.session, capture_id, 0)

    @unittest.skipUnless(os.name == "posix", "requires POSIX directory permissions")
    def test_capture_directory_is_group_writable_under_service_umask(self):
        previous_umask = os.umask(0o027)
        try:
            status = CALIBRATION.create_web_session(self.root, self.config)
        finally:
            os.umask(previous_umask)
        directory = self.root / "calibration-sessions" / status["session_id"]
        # The root agent creates sessions; the camera daemon publishes images
        # through their shared service group, without write access to metadata.
        self.assertEqual((directory / "captures").stat().st_mode & 0o777, 0o770)
        self.assertEqual(directory.stat().st_mode & 0o777, 0o750)
        self.assertEqual((directory / "configuration.json").stat().st_mode & 0o777, 0o640)
        self.assertEqual((directory / ".operation.lock").stat().st_mode & 0o777, 0o600)

    def test_partial_png_pair_never_counts_as_complete(self):
        self.fixture("partial")
        (self.directory / "captures/partial.complete.json").unlink()
        with self.assertRaisesRegex(RuntimeError, "did not commit"):
            CALIBRATION.analyze_web_capture(self.root, self.session, "partial", 0)
        status = CALIBRATION.read_web_status(self.root, self.session)
        self.assertEqual(status["state"], "failed")
        self.assertEqual(status["capture_attempts"], 1)
        self.assertNotIn("pending_capture", status)
        self.assertFalse((self.directory / "captures/partial-left.png").exists())

    def test_committed_assets_are_hashed_and_associated(self):
        manifest = self.fixture("mismatch")
        manifest["assets"]["right"]["frame"] = {**manifest["frame"], "capture_epoch": 2}
        CALIBRATION.atomic_json(self.directory / "captures/mismatch.complete.json", manifest)
        with self.assertRaisesRegex(ValueError, "right asset integrity"):
            CALIBRATION.analyze_web_capture(self.root, self.session, "mismatch", 0)
        self.fixture("tampered")
        with (self.directory / "captures/tampered-left.png").open("ab") as output:
            output.write(b"tampered")
        with self.assertRaisesRegex(ValueError, "left asset integrity"):
            CALIBRATION.analyze_web_capture(self.root, self.session, "tampered", 0)

    def test_concurrent_captures_are_serialized_and_retry_is_idempotent(self):
        self.fixture("one"); self.fixture("two")
        with patch.object(CALIBRATION, "_charuco_board", return_value=None), patch.object(
                CALIBRATION, "_detect_charuco", return_value=(None, None, None, None)), ThreadPoolExecutor(2) as executor:
            futures = [executor.submit(CALIBRATION.analyze_web_capture, self.root, self.session, capture, 0)
                       for capture in ("one", "two")]
            for future in futures:
                future.result(timeout=3)
        status = CALIBRATION.read_web_status(self.root, self.session)
        self.assertEqual({item["capture_id"] for item in status["captures"]}, {"one", "two"})
        self.assertEqual(status["capture_attempts"], 2)
        self.assertEqual(CALIBRATION.analyze_web_capture(self.root, self.session, "one", 0)["capture_attempts"], 2)

    def test_failed_attempts_remain_bounded_and_charge_reserved_storage(self):
        with patch.object(CALIBRATION, "MAX_SESSION_CAPTURES", 1):
            with self.assertRaises(RuntimeError):
                CALIBRATION.analyze_web_capture(self.root, self.session, "missing", 0)
            status = CALIBRATION.read_web_status(self.root, self.session)
            self.assertEqual(status["storage_reserved_bytes"], CALIBRATION._capture_budget(self.config))
            with self.assertRaisesRegex(ValueError, "attempt limit"):
                CALIBRATION.analyze_web_capture(self.root, self.session, "another", 0)
        with patch.object(CALIBRATION, "MAX_SESSION_BYTES", CALIBRATION._session_charge(self.directory)):
            with self.assertRaisesRegex(ValueError, "session storage quota"):
                CALIBRATION.analyze_web_capture(self.root, self.session, "quota", 0)

    def test_store_quota_includes_inflight_capture_and_timeout_cancels_request(self):
        other = CALIBRATION.create_web_session(self.root, self.config)["session_id"]
        initial = sum(CALIBRATION._session_charge(path) for path in (self.root / "calibration-sessions").iterdir())
        request_root = self.root / "requests"
        with patch.object(CALIBRATION, "MAX_SESSION_STORE_BYTES", initial + CALIBRATION._capture_budget(self.config)), ThreadPoolExecutor(1) as executor:
            future = executor.submit(CALIBRATION.analyze_web_capture, self.root, self.session, "inflight", .3, request_root)
            request = request_root / f"{self.session}--inflight.request"
            deadline = time.monotonic() + 1
            while not request.exists() and time.monotonic() < deadline:
                time.sleep(.005)
            self.assertTrue(request.exists())
            self.assertEqual(CALIBRATION.read_web_status(self.root, self.session)["state"], "capturing")
            with self.assertRaisesRegex(ValueError, "store storage quota"):
                CALIBRATION.analyze_web_capture(self.root, other, "blocked", 0)
            with self.assertRaises(RuntimeError):
                future.result(timeout=2)
            self.assertFalse(request.exists())
            self.assertFalse((self.directory / "captures/inflight.reservation").exists())

    def test_solve_errors_and_interrupted_operations_are_recoverable(self):
        with self.assertRaisesRegex(ValueError, "not enough"):
            CALIBRATION.solve_web_session(self.root, self.session)
        status = CALIBRATION.read_web_status(self.root, self.session)
        self.assertEqual(status["state"], "failed")
        self.assertEqual(status["last_error"]["operation"], "solve")
        status.update(state="capturing", pending_capture="interrupted")
        CALIBRATION.atomic_json(self.directory / "status.json", status)
        (self.directory / "captures/interrupted-left.png").write_bytes(b"partial")
        status = CALIBRATION.read_web_status(self.root, self.session)
        self.assertTrue(status["last_error"]["interrupted"])
        self.assertFalse((self.directory / "captures/interrupted-left.png").exists())

    def test_retention_skips_active_and_locked_sessions(self):
        active = CALIBRATION.create_web_session(self.root, self.config)["session_id"]
        active_directory = self.root / "calibration-sessions" / active
        status = CALIBRATION._session_status(active_directory)
        status.update(state="active", active=True, bundle_id="a" * 64)
        (self.root / "calibrations/user").mkdir(parents=True)
        (self.root / "calibrations/user/active").symlink_to("a" * 64)
        CALIBRATION._save_session(active_directory, status)
        status = CALIBRATION._session_status(self.directory)
        status["state"] = "passed"
        CALIBRATION._save_session(self.directory, status)
        with patch.object(CALIBRATION, "MAX_RETAINED_SESSIONS", 2):
            with CALIBRATION._session_operation(self.root, self.session):
                with self.assertRaisesRegex(ValueError, "retained session"):
                    CALIBRATION.create_web_session(self.root, self.config)
            new = CALIBRATION.create_web_session(self.root, self.config)
            self.assertFalse(self.directory.exists())
            self.assertTrue(active_directory.exists())
            self.assertTrue((self.root / "calibration-sessions" / new["session_id"]).exists())

    def test_interrupted_creation_staging_and_missing_status_are_cleaned(self):
        staging = self.directory.parent / ".session-staging-orphan"
        staging.mkdir()
        (staging / "configuration.json").write_text("partial")
        orphan = self.directory.parent / ("f" * 32)
        (orphan / "captures").mkdir(parents=True)
        removed = CALIBRATION.cleanup_web_sessions(self.root)
        self.assertEqual(removed, ["f" * 32])
        self.assertFalse(staging.exists())
        self.assertTrue(self.directory.exists())

    def test_startup_restores_interrupted_activation_but_allows_live_restart(self):
        from calibration_activation_test import bundle
        store = CALIBRATION.CalibrationStore(self.root)
        first = bundle(self.root, "first"); second = bundle(self.root, "second")
        previous = store.activate(first)
        candidate = json.loads((second / "manifest.json").read_text())["bundle_id"]
        shutil.copytree(second, store.calibrations / "user" / candidate)
        store._replace_active("user", candidate)
        CALIBRATION.atomic_json(store.transactions / "calibration-pending.json", {
            "namespace": "user", "bundle_id": candidate, "previous": previous, "state": "awaiting_restart"})
        with CALIBRATION.exclusive_lock(self.root / ".calibration.lock"):
            self.assertFalse(store.recover_before_start())
            self.assertEqual((store.calibrations / "user/active").resolve().name, candidate)
        self.assertTrue(store.recover_before_start())
        self.assertEqual((store.calibrations / "user/active").resolve().name, previous)
        self.assertEqual(json.loads((store.transactions / "calibration-pending.json").read_text())["state"], "aborted")


if __name__ == "__main__":
    unittest.main()
