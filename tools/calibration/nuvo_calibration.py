#!/usr/bin/env python3
"""Guided Nuvo stereo calibration and durable bundle activation."""

from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
import glob
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
from typing import Callable, Iterator
import uuid

SCHEMA = 1
REQUIRED_ASSETS = ("front_left.yaml", "front_right.yaml", "validation.json")
MAX_PROFILE_RATE_HZ = 240.0
PROFILE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
IDENTITY_VALUE = re.compile(r"^[A-Za-z0-9/][A-Za-z0-9._:/@+-]{0,255}$")
SESSION_ID = re.compile(r"^[a-f0-9]{32}$")
CAPTURE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
CHARUCO_DICTIONARIES = {
    "DICT_4X4_50", "DICT_4X4_100", "DICT_5X5_50", "DICT_5X5_100",
    "DICT_5X5_250", "DICT_6X6_100", "DICT_6X6_250",
}
# The supported printed target uses the post-4.6 pattern and row-major corner
# coordinates. Older even-row targets are different and must not be guessed.
CHARUCO_LAYOUT = "opencv-4.6"
MAX_SESSION_CAPTURES = 400
MAX_SESSION_BYTES = 512 * 1024 * 1024
MAX_SESSION_STORE_BYTES = 2 * 1024 * 1024 * 1024
MAX_RETAINED_SESSIONS = 16
SESSION_RETENTION_SECONDS = 7 * 24 * 60 * 60
IDLE_SESSION_SECONDS = 24 * 60 * 60
BUSY_SESSION_STATES = {"capturing", "solving", "activating"}


def _capture_budget(configuration: dict) -> int:
    profile = configuration["profile"]
    pixels = profile["image_width"] * profile["image_height"]
    # PNG's deflate overhead plus filter rows, metadata, and analysis JSON.
    return 2 * (pixels + pixels // 100 + profile["image_height"] + 65536) + 65536


def _session_base_budget(configuration: dict) -> int:
    # Two current RGB JPEG previews, configuration, status and solved bundle.
    profile = configuration["profile"]
    return 6 * profile["image_width"] * profile["image_height"] + 512 * 1024


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fsync_directory(path: Path) -> None:
    if os.name != "posix":
        return
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try: os.fsync(descriptor)
    finally: os.close(descriptor)


def atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            # The daemon and validation worker share read access through nuvo-video.
            # mkstemp defaults to 0600; keep existing permissions or create 0640.
            os.chmod(temporary, path.stat().st_mode & 0o777 if path.exists() else 0o640)
            json.dump(value, output, sort_keys=True, separators=(",", ":"))
            output.write("\n"); output.flush(); os.fsync(output.fileno())
        os.replace(temporary, path); fsync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_bytes(path: Path, value: bytes) -> None:
    descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            os.chmod(temporary, 0o640)
            output.write(value); output.flush(); os.fsync(output.fileno())
        os.replace(temporary, path); fsync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


@contextmanager
def exclusive_lock(path: Path) -> Iterator[None]:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        if os.name != "posix":
            raise RuntimeError("durable calibration activation is supported only on POSIX targets")
        import fcntl
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        os.close(descriptor)


@contextmanager
def try_lock(path: Path, *, create: bool = False) -> Iterator[bool]:
    """Cleanup and startup recovery never wait while holding another lock."""
    if os.name != "posix":
        raise RuntimeError("calibration operations require POSIX file locks")
    import fcntl
    descriptor = os.open(path, os.O_RDWR | (os.O_CREAT if create else 0), 0o600)
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            yield False
        else:
            yield True
    finally:
        os.close(descriptor)


def verify_factory_signature(manifest: dict, material: bytes,
                             trusted_keys: Path | None) -> None:
    signature = manifest.get("signature")
    if not isinstance(signature, dict) or signature.get("algorithm") != "ed25519" or not signature.get("key_id") or not signature.get("value"):
        raise ValueError("factory calibration requires an Ed25519 signature")
    if trusted_keys is None or not trusted_keys.is_file():
        raise ValueError("factory calibration trust store is unavailable")
    trust = json.loads(trusted_keys.read_text(encoding="utf-8"))
    if trust.get("schema_version") != 1 or not isinstance(trust.get("keys"), dict):
        raise ValueError("factory calibration trust store is malformed")
    encoded_key = trust["keys"].get(signature["key_id"])
    if not isinstance(encoded_key, str):
        raise ValueError("factory calibration signing key is not trusted")
    try:
        public_key = base64.b64decode(encoded_key, validate=True)
        signed = base64.b64decode(signature["value"], validate=True)
        if len(public_key) != 32 or len(signed) != 64:
            raise ValueError("factory signature encoding has invalid length")
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
        Ed25519PublicKey.from_public_bytes(public_key).verify(signed, material)
    except ImportError as error:
        raise RuntimeError("python3-cryptography is required for factory calibration verification") from error
    except ValueError:
        raise
    except Exception as error:
        raise ValueError("factory calibration signature verification failed") from error


def validate_bundle(bundle: Path, *, require_factory_signature: bool = False,
                    trusted_keys: Path | None = None) -> dict:
    manifest_path = bundle / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != SCHEMA:
        raise ValueError("unsupported calibration bundle schema")
    if manifest.get("authority") not in {"user_validated", "factory_validated"}:
        raise ValueError("bundle authority is not activatable")
    if not all(manifest.get(key) for key in ("bundle_id", "unit_id", "left_camera_id", "right_camera_id", "preset_id")):
        raise ValueError("bundle identity is incomplete")
    if manifest["left_camera_id"] == manifest["right_camera_id"]:
        raise ValueError("logical cameras must have different stable identities")
    profile = manifest.get("profile")
    if profile is not None:
        if (not isinstance(profile, dict) or
                not isinstance(profile.get("sensor_mode"), str) or
                not profile["sensor_mode"].strip()):
            raise ValueError("calibration profile geometry is malformed")
        if not all(isinstance(profile.get(key), int) and not isinstance(profile.get(key), bool)
                   and profile[key] > 0 for key in ("image_width", "image_height")):
            raise ValueError("calibration profile dimensions are malformed")
        rates = profile.get("frame_rates_hz")
        if (not isinstance(rates, list) or not rates or
                any(not isinstance(rate, (int, float)) or isinstance(rate, bool)
                    or rate <= 0 or rate > MAX_PROFILE_RATE_HZ for rate in rates)):
            raise ValueError("calibration profile frame-rate set is malformed")
        if len(set(float(rate) for rate in rates)) != len(rates):
            raise ValueError("calibration profile repeats a frame rate")
    assets = manifest.get("assets")
    if not isinstance(assets, dict) or set(assets) != set(REQUIRED_ASSETS):
        raise ValueError("bundle asset manifest is incomplete")
    for name, expected in assets.items():
        path = bundle / name
        if not path.is_file() or sha256(path) != expected:
            raise ValueError(f"bundle asset hash mismatch: {name}")
    identity_material = json.dumps(
        {key: value for key, value in manifest.items() if key not in {"bundle_id", "signature"}},
        sort_keys=True, separators=(",", ":"),
    ).encode()
    if hashlib.sha256(identity_material).hexdigest() != manifest["bundle_id"]:
        raise ValueError("bundle content identity is invalid")
    if require_factory_signature or manifest["authority"] == "factory_validated":
        verify_factory_signature(manifest, identity_material, trusted_keys)
    validation = json.loads((bundle / "validation.json").read_text(encoding="utf-8"))
    if validation.get("status") != "pass":
        raise ValueError("calibration validation did not pass")
    return manifest


class CalibrationStore:
    def __init__(self, root: Path, factory_trust: Path | None = None):
        self.root = root
        self.factory_trust = factory_trust
        self.calibrations = root / "calibrations"
        self.transactions = root / "transactions"

    def recover_before_start(self) -> bool:
        # Activation holds this lock through systemctl restart. Its ExecStartPre
        # must allow that live transaction to load the staged selection instead
        # of deadlocking or treating it as an interrupted operation. A dead
        # activator releases flock automatically, so boot recovery can own it.
        self.root.mkdir(parents=True, exist_ok=True)
        with try_lock(self.root / ".calibration.lock", create=True) as acquired:
            if not acquired:
                return False
            recovered = self._recover_pending(None)
        if recovered:
            for directory in (self.root / "calibration-sessions").glob("*"):
                if SESSION_ID.fullmatch(directory.name) and directory.is_dir():
                    try:
                        with try_lock(directory / ".operation.lock") as acquired:
                            if acquired:
                                _recover_session(directory)
                    except FileNotFoundError:
                        pass
        return recovered

    def _replace_active(self, namespace: str, bundle_id: str | None) -> None:
        if namespace not in {"user", "factory"}:
            raise ValueError("calibration namespace must be user or factory")
        active = self.calibrations / namespace / "active"
        active.parent.mkdir(parents=True, exist_ok=True)
        if bundle_id is None:
            active.unlink(missing_ok=True)
            fsync_directory(active.parent)
            return
        if not isinstance(bundle_id, str) or not re.fullmatch(r"[a-f0-9]{64}", bundle_id):
            raise ValueError("unsafe active calibration identity")
        validate_bundle(self.calibrations / namespace / bundle_id,
                        require_factory_signature=namespace == "factory",
                        trusted_keys=self.factory_trust)
        link = active.with_name(".active.tmp")
        link.unlink(missing_ok=True)
        os.symlink(bundle_id, link); os.replace(link, active)
        fsync_directory(active.parent)

    def _recover_pending(self, restart: Callable[[str | None], None] | None) -> bool:
        # A killed process leaves the previous selection in the durable journal.
        # Restore it before admitting another transition. No committed activation
        # is inferred merely from an active symlink or a started service.
        recovered = False
        for journal in sorted(self.transactions.glob("calibration-*.json")):
            transaction = json.loads(journal.read_text(encoding="utf-8"))
            if transaction.get("state") not in {"staging", "awaiting_restart", "restoring"}:
                continue
            recovered = True
            namespace = transaction["namespace"]
            self._replace_active(namespace, transaction.get("previous"))
            transaction["state"] = "restoring"
            atomic_json(journal, transaction)
            if restart is not None:
                restart(transaction.get("previous"))
            transaction["state"] = "aborted"
            transaction["recovered_unix_ns"] = time.time_ns()
            atomic_json(journal, transaction)
        return recovered

    def _complete_transition(self, journal: Path, transaction: dict,
                             restart: Callable[[str | None], None] | None) -> None:
        transaction["state"] = "awaiting_restart"
        atomic_json(journal, transaction)
        try:
            self._replace_active(transaction["namespace"], transaction["bundle_id"])
            if restart is not None:
                restart(transaction["bundle_id"])
            transaction["state"] = "committed"
            transaction["committed_unix_ns"] = time.time_ns()
            atomic_json(journal, transaction)
        except Exception as error:
            transaction["state"] = "restoring"
            transaction["error"] = str(error)
            atomic_json(journal, transaction)
            self._replace_active(transaction["namespace"], transaction.get("previous"))
            try:
                if restart is not None:
                    restart(transaction.get("previous"))
            except Exception as restore_error:
                transaction["restore_error"] = str(restore_error)
                atomic_json(journal, transaction)
                raise RuntimeError(
                    f"calibration startup failed: {error}; previous selection restored "
                    f"but service recovery failed: {restore_error}") from error
            transaction["state"] = "aborted"
            transaction["restored_unix_ns"] = time.time_ns()
            atomic_json(journal, transaction)
            raise RuntimeError(
                f"calibration startup failed: {error}; previous selection and service restored") from error

    def activate(self, source: Path, namespace: str = "user", *,
                 restart: Callable[[str | None], None] | None = None) -> str:
        if namespace not in {"user", "factory"}:
            raise ValueError("calibration namespace must be user or factory")
        manifest = validate_bundle(source,
                                   require_factory_signature=namespace == "factory",
                                   trusted_keys=self.factory_trust)
        if (namespace == "factory") != (manifest["authority"] == "factory_validated"):
            raise ValueError("bundle authority and namespace disagree")
        bundle_id = manifest["bundle_id"]
        destination = self.calibrations / namespace / bundle_id
        active = self.calibrations / namespace / "active"
        with exclusive_lock(self.root / ".calibration.lock"):
            if self._recover_pending(restart) and restart is not None:
                raise RuntimeError("interrupted calibration transaction restored; retry the requested change")
            if (restart is not None and namespace == "user" and
                    (self.calibrations / "factory" / "active" / "manifest.json").is_file()):
                raise ValueError("factory calibration has runtime precedence; user activation would not load")
            self.transactions.mkdir(parents=True, exist_ok=True)
            transaction = {
                "schema_version": 1, "operation": "calibration_activate",
                "namespace": namespace, "bundle_id": bundle_id,
                "previous": os.readlink(active) if active.is_symlink() else None,
                "state": "staging", "started_unix_ns": time.time_ns(),
            }
            journal = self.transactions / f"calibration-{time.time_ns()}.json"
            atomic_json(journal, transaction)
            if not destination.exists():
                destination.parent.mkdir(parents=True, exist_ok=True)
                staging = Path(tempfile.mkdtemp(prefix=f".{bundle_id}.", dir=destination.parent))
                try:
                    os.chmod(staging, 0o750)
                    for name in ("manifest.json", *REQUIRED_ASSETS):
                        shutil.copyfile(source / name, staging / name)
                        with (staging / name).open("rb") as copied: os.fsync(copied.fileno())
                    fsync_directory(staging)
                    os.replace(staging, destination); fsync_directory(destination.parent)
                finally:
                    if staging.exists(): shutil.rmtree(staging)
            validate_bundle(destination,
                            require_factory_signature=namespace == "factory",
                            trusted_keys=self.factory_trust)
            self._complete_transition(journal, transaction, restart)
        return bundle_id

    def rollback(self, namespace: str = "user", *,
                 restart: Callable[[str | None], None] | None = None) -> str:
        if namespace not in {"user", "factory"}:
            raise ValueError("calibration namespace must be user or factory")
        with exclusive_lock(self.root / ".calibration.lock"):
            if self._recover_pending(restart) and restart is not None:
                raise RuntimeError("interrupted calibration transaction restored; retry the requested change")
            if (restart is not None and namespace == "user" and
                    (self.calibrations / "factory" / "active" / "manifest.json").is_file()):
                raise ValueError("factory calibration has runtime precedence; user rollback would not load")
            active = self.calibrations / namespace / "active"
            current = os.readlink(active) if active.is_symlink() else None
            journals = sorted(self.transactions.glob("calibration-*.json"), reverse=True)
            for path in journals:
                record = json.loads(path.read_text(encoding="utf-8"))
                previous = record.get("previous")
                if (record.get("namespace") == namespace and
                        record.get("state") == "committed" and previous and
                        record.get("bundle_id") == current and previous != current):
                    destination = self.calibrations / namespace / previous
                    validate_bundle(destination,
                                    require_factory_signature=namespace == "factory",
                                    trusted_keys=self.factory_trust)
                    transaction = {
                        "schema_version":1, "operation":"calibration_rollback",
                        "namespace":namespace, "bundle_id":previous,
                        "previous":current, "state":"staging",
                        "started_unix_ns":time.time_ns(),
                    }
                    journal = self.transactions / f"calibration-{time.time_ns()}.json"
                    atomic_json(journal, transaction)
                    self._complete_transition(journal, transaction, restart)
                    return previous
            raise RuntimeError("no validated previous calibration is available")


def matrix_yaml(name: str, matrix) -> dict:
    import numpy as np
    value = np.asarray(matrix)
    return {"rows": int(value.shape[0]), "cols": int(value.shape[1]),
            "data": [float(x) for x in value.reshape(-1)]}


def load_matrix_plan(path: Path) -> dict:
    """Load and strictly validate a reproducible multi-profile calibration plan."""
    plan = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(plan, dict) or plan.get("schema_version") != SCHEMA:
        raise ValueError("unsupported calibration matrix schema")
    for key in ("unit_id", "left_camera_id", "right_camera_id"):
        if not isinstance(plan.get(key), str) or not plan[key].strip():
            raise ValueError(f"calibration matrix requires {key}")
    if plan["left_camera_id"] == plan["right_camera_id"]:
        raise ValueError("calibration matrix camera identities must differ")
    target = plan.get("target")
    if not isinstance(target, dict) or target.get("type") != "chessboard":
        raise ValueError("calibration matrix requires a chessboard target")
    for key in ("columns", "rows"):
        if not isinstance(target.get(key), int) or isinstance(target.get(key), bool) or target[key] < 3:
            raise ValueError(f"chessboard {key} must be an integer of at least 3")
    if (not isinstance(target.get("square_m"), (int, float)) or
            isinstance(target.get("square_m"), bool) or target["square_m"] <= 0):
        raise ValueError("chessboard square_m must be positive")
    profiles = plan.get("profiles")
    if not isinstance(profiles, list) or not profiles:
        raise ValueError("calibration matrix requires at least one profile")
    identifiers: set[str] = set()
    for profile in profiles:
        if not isinstance(profile, dict):
            raise ValueError("each calibration matrix profile must be an object")
        for key in ("profile_id", "sensor_mode", "left", "right"):
            if not isinstance(profile.get(key), str) or not profile[key].strip():
                raise ValueError(f"calibration profile requires {key}")
        if profile["profile_id"] in identifiers:
            raise ValueError(f"duplicate calibration profile: {profile['profile_id']}")
        if not PROFILE_ID.fullmatch(profile["profile_id"]):
            raise ValueError(f"profile {profile['profile_id']} has an unsafe identifier")
        identifiers.add(profile["profile_id"])
        for key in ("width", "height"):
            if not isinstance(profile.get(key), int) or isinstance(profile.get(key), bool) or profile[key] <= 0:
                raise ValueError(f"calibration profile {key} must be a positive integer")
        rates = profile.get("frame_rates_hz")
        if (not isinstance(rates, list) or not rates or
                any(not isinstance(rate, (int, float)) or isinstance(rate, bool)
                    or rate <= 0 or rate > MAX_PROFILE_RATE_HZ for rate in rates)):
            raise ValueError(f"profile {profile['profile_id']} has an invalid frame-rate set")
        if len(set(float(rate) for rate in rates)) != len(rates):
            raise ValueError(f"profile {profile['profile_id']} repeats a frame rate")
        minimum = profile.get("minimum_pairs", plan.get("minimum_pairs", 20))
        if not isinstance(minimum, int) or isinstance(minimum, bool) or minimum < 10:
            raise ValueError(f"profile {profile['profile_id']} minimum_pairs must be at least 10")
        for key, fallback in (("maximum_rms_px", .5),
                              ("maximum_epipolar_p95_px", .5)):
            threshold = profile.get(key, plan.get(key, fallback))
            if (not isinstance(threshold, (int, float)) or isinstance(threshold, bool)
                    or threshold <= 0 or threshold > 10):
                raise ValueError(f"profile {profile['profile_id']} has an invalid {key}")
    return plan


def capture_pair_key(path: str) -> str:
    stem = Path(path).stem.lower()
    stem = re.sub(r"(^|[_-])(front[_-]?)?(left|right|l|r)(?=[_-]|$)", r"\1", stem)
    return stem.strip("_-")


def solve(args: argparse.Namespace) -> Path:
    import cv2
    import numpy as np
    import yaml
    frame_rates = list(getattr(args, "frame_rates_hz", []))
    if (not frame_rates or
            any(rate <= 0 or rate > MAX_PROFILE_RATE_HZ for rate in frame_rates) or
            len(set(float(rate) for rate in frame_rates)) != len(frame_rates)):
        raise ValueError("frame-rates-hz must contain unique positive rates up to 240 Hz")
    left_paths, right_paths = sorted(glob.glob(args.left)), sorted(glob.glob(args.right))
    if len(left_paths) != len(right_paths) or len(left_paths) < args.minimum_pairs:
        raise ValueError("left/right calibration sets must have equal counts above the minimum")
    board = (args.columns, args.rows)
    object_points = np.zeros((args.rows * args.columns, 3), np.float32)
    object_points[:, :2] = np.mgrid[0:args.columns, 0:args.rows].T.reshape(-1, 2) * args.square_m
    objects, left_points, right_points = [], [], []
    image_size = None
    for left_path, right_path in zip(left_paths, right_paths):
        if capture_pair_key(left_path) != capture_pair_key(right_path):
            raise ValueError(
                f"left/right capture indices do not match: {Path(left_path).name} / "
                f"{Path(right_path).name}")
        left = cv2.imread(left_path, cv2.IMREAD_GRAYSCALE); right = cv2.imread(right_path, cv2.IMREAD_GRAYSCALE)
        if left is None or right is None or left.shape != right.shape:
            continue
        pair_size = (left.shape[1], left.shape[0])
        if image_size is not None and pair_size != image_size:
            raise ValueError("calibration capture set mixes image dimensions")
        image_size = pair_size
        ok_left, corners_left = cv2.findChessboardCornersSB(left, board)
        ok_right, corners_right = cv2.findChessboardCornersSB(right, board)
        if ok_left and ok_right:
            objects.append(object_points.copy()); left_points.append(corners_left); right_points.append(corners_right)
    if len(objects) < args.minimum_pairs or image_size is None:
        raise ValueError("not enough synchronized pairs contain a detectable target")
    expected_width = getattr(args, "expected_width", None)
    expected_height = getattr(args, "expected_height", None)
    if (expected_width is None) != (expected_height is None):
        raise ValueError("expected calibration width and height must be supplied together")
    if expected_width is not None and image_size != (expected_width, expected_height):
        raise ValueError(
            f"captured calibration size {image_size[0]}x{image_size[1]} does not match "
            f"declared profile {expected_width}x{expected_height}")
    left_rms, kl, dl, _, _ = cv2.calibrateCamera(objects, left_points, image_size, None, None)
    right_rms, kr, dr, _, _ = cv2.calibrateCamera(objects, right_points, image_size, None, None)
    stereo_rms, kl, dl, kr, dr, rotation, translation, _, _ = cv2.stereoCalibrate(
        objects, left_points, right_points, kl, dl, kr, dr, image_size,
        flags=cv2.CALIB_FIX_INTRINSIC,
        criteria=(cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 200, 1e-8))
    rl, rr, pl, pr, q, roi_l, roi_r = cv2.stereoRectify(
        kl, dl, kr, dr, image_size, rotation, translation, flags=cv2.CALIB_ZERO_DISPARITY, alpha=0)
    baseline = float(np.linalg.norm(translation))
    vertical_errors = []
    for lp, rp in zip(left_points, right_points):
        l = cv2.undistortPoints(lp, kl, dl, R=rl, P=pl)
        r = cv2.undistortPoints(rp, kr, dr, R=rr, P=pr)
        vertical_errors.extend(np.abs(l[:,0,1] - r[:,0,1]))
    p95 = float(np.percentile(vertical_errors, 95))
    passed = stereo_rms <= args.maximum_rms and p95 <= args.maximum_epipolar_p95
    authority = "user_validated" if passed else "provisional_recalibration_required"
    output = Path(args.output); output.mkdir(parents=True, exist_ok=False)
    common = {"calibration_status": authority, "baseline_m": baseline,
              "image_width": image_size[0], "image_height": image_size[1],
              "distortion_model":"plumb_bob"}
    for name, camera_name, k, d, r, p in (
        ("front_left.yaml", "front_left", kl, dl, rl, pl),
        ("front_right.yaml", "front_right", kr, dr, rr, pr)):
        document = {**common, "camera_name":camera_name,
                    "camera_matrix":matrix_yaml("K", k),
                    "distortion_coefficients":matrix_yaml("D", d.reshape(1,-1)),
                    "rectification_matrix":matrix_yaml("R", r),
                    "projection_matrix":matrix_yaml("P", p)}
        with (output/name).open("x", encoding="utf-8") as target: yaml.safe_dump(document,target,sort_keys=False)
    validation = {"schema_version":1, "status":"pass" if passed else "fail",
                  "accepted_pairs":len(objects), "left_rms_px":left_rms,
                  "right_rms_px":right_rms, "stereo_rms_px":stereo_rms,
                  "rectified_vertical_error_p95_px":p95,
                  "thresholds":{"maximum_rms_px":args.maximum_rms,
                                "maximum_epipolar_p95_px":args.maximum_epipolar_p95}}
    atomic_json(output/"validation.json", validation)
    assets = {name:sha256(output/name) for name in REQUIRED_ASSETS}
    manifest = {"schema_version":SCHEMA, "unit_id":args.unit_id,
                "left_camera_id":args.left_camera_id, "right_camera_id":args.right_camera_id,
                "preset_id":args.preset_id, "authority":authority, "assets":assets,
                "profile":{"sensor_mode":getattr(args, "sensor_mode", "unspecified"),
                           "image_width":image_size[0], "image_height":image_size[1],
                           "frame_rates_hz":sorted(float(rate) for rate in frame_rates)},
                "provenance":{"solver":"opencv", "opencv_version":cv2.__version__,
                              "created_unix_ns":time.time_ns(), "target":"chessboard",
                              "square_m":args.square_m}}
    identity = hashlib.sha256(json.dumps(manifest,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    manifest["bundle_id"] = identity; atomic_json(output/"manifest.json", manifest)
    if not passed: raise RuntimeError(f"calibration did not meet thresholds; evidence saved at {output}")
    return output


def solve_matrix(args: argparse.Namespace) -> Path:
    plan_path = Path(args.plan).resolve()
    plan = load_matrix_plan(plan_path)
    output_root = Path(args.output_root)
    output_root.mkdir(parents=True, exist_ok=False)
    target = plan["target"]
    base = plan_path.parent
    results = []
    for profile in plan["profiles"]:
        output = output_root / profile["profile_id"]
        left = Path(profile["left"])
        right = Path(profile["right"])
        options = argparse.Namespace(
            left=str(left if left.is_absolute() else base / left),
            right=str(right if right.is_absolute() else base / right),
            columns=target["columns"], rows=target["rows"],
            square_m=float(target["square_m"]),
            minimum_pairs=int(profile.get("minimum_pairs", plan.get("minimum_pairs", 20))),
            maximum_rms=float(profile.get("maximum_rms_px", plan.get("maximum_rms_px", .5))),
            maximum_epipolar_p95=float(profile.get(
                "maximum_epipolar_p95_px", plan.get("maximum_epipolar_p95_px", .5))),
            unit_id=plan["unit_id"], left_camera_id=plan["left_camera_id"],
            right_camera_id=plan["right_camera_id"],
            preset_id=profile["profile_id"], output=str(output),
            sensor_mode=profile["sensor_mode"],
            expected_width=profile["width"], expected_height=profile["height"],
            frame_rates_hz=profile["frame_rates_hz"],
        )
        result = {"profile_id": profile["profile_id"],
                  "sensor_mode": profile["sensor_mode"],
                  "width": profile["width"], "height": profile["height"],
                  "frame_rates_hz": profile["frame_rates_hz"],
                  "bundle": profile["profile_id"]}
        try:
            solve(options)
            manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
            validation = json.loads((output / "validation.json").read_text(encoding="utf-8"))
            result.update(status="pass", bundle_id=manifest["bundle_id"], validation=validation)
        except Exception as error:
            result.update(status="fail", error=str(error))
            validation_path = output / "validation.json"
            if validation_path.is_file():
                result["validation"] = json.loads(validation_path.read_text(encoding="utf-8"))
        results.append(result)
    index = {"schema_version": SCHEMA, "operation": "calibration_profile_matrix",
             "unit_id": plan["unit_id"], "left_camera_id": plan["left_camera_id"],
             "right_camera_id": plan["right_camera_id"],
             "created_unix_ns": time.time_ns(),
             "status": "pass" if all(item["status"] == "pass" for item in results) else "fail",
             "profiles": results}
    index_path = output_root / "calibration-matrix.json"
    atomic_json(index_path, index)
    if index["status"] != "pass":
        raise RuntimeError(f"one or more calibration profiles failed; evidence saved at {index_path}")
    return index_path


def _finite_number(value: object, name: str, minimum: float,
                   maximum: float) -> float:
    if (not isinstance(value, (int, float)) or isinstance(value, bool) or
            not math.isfinite(float(value)) or not minimum <= float(value) <= maximum):
        raise ValueError(f"{name} must be between {minimum} and {maximum}")
    return float(value)


def validate_web_configuration(value: object) -> dict:
    """Validate the complete, versioned browser-calibration contract."""
    if not isinstance(value, dict) or value.get("schema_version") != SCHEMA:
        raise ValueError("unsupported calibration session schema")
    expected = {"schema_version", "identity", "profile", "target", "thresholds"}
    if set(value) != expected:
        raise ValueError("calibration session fields are incomplete or unknown")
    identity = value.get("identity")
    if not isinstance(identity, dict) or set(identity) != {
            "unit_id", "left_camera_id", "right_camera_id"}:
        raise ValueError("calibration identity fields are incomplete or unknown")
    for key in ("unit_id", "left_camera_id", "right_camera_id"):
        if (not isinstance(identity.get(key), str) or
                not IDENTITY_VALUE.fullmatch(identity[key])):
            raise ValueError(f"calibration {key} is unsafe")
    if identity["left_camera_id"] == identity["right_camera_id"]:
        raise ValueError("logical camera identities must differ")

    profile = value.get("profile")
    if not isinstance(profile, dict) or set(profile) != {
            "preset_id", "sensor_mode", "image_width", "image_height",
            "frame_rates_hz", "baseline_m"}:
        raise ValueError("calibration profile fields are incomplete or unknown")
    if not isinstance(profile.get("preset_id"), str) or not PROFILE_ID.fullmatch(profile["preset_id"]):
        raise ValueError("calibration preset_id is unsafe")
    if (not isinstance(profile.get("sensor_mode"), str) or
            not PROFILE_ID.fullmatch(profile["sensor_mode"])):
        raise ValueError("calibration sensor_mode is unsafe")
    for key in ("image_width", "image_height"):
        if (not isinstance(profile.get(key), int) or isinstance(profile[key], bool) or
                not 64 <= profile[key] <= 8192):
            raise ValueError(f"profile {key} is out of range")
    rates = profile.get("frame_rates_hz")
    if (not isinstance(rates, list) or not rates or len(rates) > 16 or
            any(not isinstance(rate, (int, float)) or isinstance(rate, bool) or
                not 0 < float(rate) <= MAX_PROFILE_RATE_HZ for rate in rates) or
            len(set(float(rate) for rate in rates)) != len(rates)):
        raise ValueError("profile frame_rates_hz is invalid")
    _finite_number(profile.get("baseline_m"), "profile baseline_m", .01, 2.0)

    target = value.get("target")
    target_fields = {"type", "squares_x", "squares_y", "square_m", "marker_m", "dictionary"}
    if (not isinstance(target, dict) or
            set(target) not in (target_fields, target_fields | {"layout_version"})):
        raise ValueError("calibration target fields are incomplete or unknown")
    if target.get("type") != "charuco":
        raise ValueError("browser calibration requires a ChArUco target")
    for key in ("squares_x", "squares_y"):
        if (not isinstance(target.get(key), int) or isinstance(target[key], bool) or
                not 4 <= target[key] <= 30):
            raise ValueError(f"target {key} is out of range")
    square_m = _finite_number(target.get("square_m"), "target square_m", .001, .5)
    marker_m = _finite_number(target.get("marker_m"), "target marker_m", .0005, .49)
    if marker_m >= square_m:
        raise ValueError("target marker_m must be smaller than square_m")
    if target.get("dictionary") not in CHARUCO_DICTIONARIES:
        raise ValueError("unsupported ChArUco dictionary")
    if target.get("layout_version", CHARUCO_LAYOUT) != CHARUCO_LAYOUT:
        raise ValueError("unsupported ChArUco layout_version; use the OpenCV 4.6+ printed target")

    thresholds = value.get("thresholds")
    if not isinstance(thresholds, dict) or set(thresholds) != {
            "minimum_pairs", "minimum_shared_corners", "maximum_rms_px",
            "maximum_epipolar_p95_px", "baseline_tolerance_m",
            "minimum_coverage"}:
        raise ValueError("calibration threshold fields are incomplete or unknown")
    if (not isinstance(thresholds.get("minimum_pairs"), int) or
            isinstance(thresholds["minimum_pairs"], bool) or
            not 12 <= thresholds["minimum_pairs"] <= 200):
        raise ValueError("minimum_pairs must be in 12..200")
    maximum_corners = (target["squares_x"] - 1) * (target["squares_y"] - 1)
    if (not isinstance(thresholds.get("minimum_shared_corners"), int) or
            isinstance(thresholds["minimum_shared_corners"], bool) or
            not 6 <= thresholds["minimum_shared_corners"] <= maximum_corners):
        raise ValueError("minimum_shared_corners is incompatible with the target")
    _finite_number(thresholds.get("maximum_rms_px"), "maximum_rms_px", .05, 5.0)
    _finite_number(thresholds.get("maximum_epipolar_p95_px"),
                   "maximum_epipolar_p95_px", .05, 5.0)
    _finite_number(thresholds.get("baseline_tolerance_m"),
                   "baseline_tolerance_m", .0001, .1)
    _finite_number(thresholds.get("minimum_coverage"), "minimum_coverage", .1, 1.0)
    configuration = json.loads(json.dumps(value))
    configuration["target"]["layout_version"] = CHARUCO_LAYOUT
    return configuration


def _session_path(state_root: Path, session_id: str) -> Path:
    if not SESSION_ID.fullmatch(session_id):
        raise ValueError("unsafe calibration session identifier")
    return state_root / "calibration-sessions" / session_id


def _session_status(directory: Path) -> dict:
    path = directory / "status.json"
    if not path.is_file():
        raise ValueError("calibration session does not exist")
    return json.loads(path.read_text(encoding="utf-8"))


def _save_session(directory: Path, status: dict) -> None:
    status["updated_unix_ns"] = time.time_ns()
    atomic_json(directory / "status.json", status)


def _remove_capture(directory: Path, capture_id: str) -> None:
    if not CAPTURE_ID.fullmatch(capture_id):
        return
    for suffix in ("-left.png", "-right.png", ".json", ".complete.json",
                   ".reservation", "-analysis.json"):
        (directory / "captures" / f"{capture_id}{suffix}").unlink(missing_ok=True)


def _recover_session(directory: Path) -> dict:
    status = _session_status(directory)
    if status.get("state") not in BUSY_SESSION_STATES:
        return status
    operation = status["state"]
    if operation == "capturing":
        _remove_capture(directory, status.get("pending_capture", ""))
    # The caller owns the session lock. A live operation cannot be mistaken for
    # an interrupted one, including during a long solve or service restart.
    status.pop("pending_capture", None)
    status["state"] = ("passed" if operation == "activating" and
                       (status.get("validation") or {}).get("status") == "pass"
                       else "failed")
    status["active"] = False
    status["last_error"] = {"operation": operation, "interrupted": True,
                            "message": f"Interrupted calibration {operation}; retry the operation."}
    status["message"] = status["last_error"]["message"]
    _save_session(directory, status)
    return status


@contextmanager
def _session_operation(state_root: Path, session_id: str) -> Iterator[Path]:
    directory = _session_path(state_root, session_id)
    if not directory.is_dir() or directory.is_symlink():
        raise ValueError("calibration session does not exist")
    # Do not recreate a lock after cleanup deleted its session. Old sessions
    # created before operation locks are upgraded under the global quota lock.
    lock_path = directory / ".operation.lock"
    if not lock_path.exists():
        with exclusive_lock(state_root / ".calibration-sessions.lock"):
            if not directory.is_dir():
                raise ValueError("calibration session does not exist")
            lock_path.touch(mode=0o600, exist_ok=True)
    descriptor = os.open(lock_path, os.O_RDWR)
    try:
        import fcntl
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        if not directory.is_dir():
            raise ValueError("calibration session was removed by retention cleanup")
        _recover_session(directory)
        yield directory
    finally:
        os.close(descriptor)


def _session_charge(directory: Path) -> int:
    actual = sum(path.stat().st_size for path in directory.rglob("*")
                 if path.is_file() and not path.is_symlink())
    try:
        status = _session_status(directory)
        configuration = json.loads((directory / "configuration.json").read_text())
        reserved = status.get("storage_reserved_bytes", 0) + _session_base_budget(configuration)
    except (OSError, ValueError, KeyError):
        reserved = actual
    return max(actual, reserved)


def _cleanup_sessions_locked(state_root: Path, *, needed_bytes: int = 0,
                             need_slot: bool = False) -> list[str]:
    root = state_root / "calibration-sessions"
    # Creation holds the quota lock until its staging directory is renamed.
    # Therefore any staging directory found by another owner is interrupted.
    for staging in root.glob(".session-staging-*"):
        if staging.is_dir() and not staging.is_symlink():
            shutil.rmtree(staging)
    directories = [path for path in root.glob("*") if path.is_dir() and
                   not path.is_symlink() and SESSION_ID.fullmatch(path.name)]
    now = time.time_ns()
    removed = []
    def age(path):
        try:
            value = _session_status(path)
            return value.get("updated_unix_ns", value.get("created_unix_ns", 0))
        except (OSError, ValueError):
            return 0
    for directory in sorted(directories, key=age):
        try:
            with try_lock(directory / ".operation.lock", create=True) as acquired:
                if not acquired:
                    continue
                if not (directory / "status.json").is_file() or not (directory / "configuration.json").is_file():
                    shutil.rmtree(directory)
                    directories.remove(directory)
                    removed.append(directory.name)
                    continue
                status = _recover_session(directory)
                active = state_root / "calibrations/user/active"
                factory = state_root / "calibrations/factory/active/manifest.json"
                selected = active.is_symlink() and os.readlink(active) == status.get("bundle_id") and not factory.is_file()
                if status.get("state") == "active" and not selected:
                    status.update(state="passed", active=False)
                    _save_session(directory, status)
                if status.get("active") or status.get("state") == "active":
                    continue
                updated = status.get("updated_unix_ns", status.get("created_unix_ns", now))
                seconds = (now - updated) / 1e9
                terminal = status.get("state") in {"passed", "failed"}
                expired = seconds >= (SESSION_RETENTION_SECONDS if terminal else IDLE_SESSION_SECONDS)
                capacity_needed = (len(directories) + int(need_slot) > MAX_RETAINED_SESSIONS or
                                   sum(_session_charge(path) for path in directories) + needed_bytes >
                                   MAX_SESSION_STORE_BYTES)
                if expired or (terminal and capacity_needed):
                    shutil.rmtree(directory)
                    directories.remove(directory)
                    removed.append(directory.name)
        except FileNotFoundError:
            continue
    fsync_directory(root) if root.is_dir() else None
    return removed


def cleanup_web_sessions(state_root: Path) -> list[str]:
    with exclusive_lock(state_root / ".calibration-sessions.lock"):
        return _cleanup_sessions_locked(state_root)


def create_web_session(state_root: Path, configuration: object) -> dict:
    configuration = validate_web_configuration(configuration)
    base_budget = _session_base_budget(configuration)
    if base_budget + configuration["thresholds"]["minimum_pairs"] * _capture_budget(configuration) > MAX_SESSION_BYTES:
        raise ValueError("profile minimum_pairs exceeds the calibration session storage quota")
    session_id = uuid.uuid4().hex
    directory = _session_path(state_root, session_id)
    status = {
        "schema_version": SCHEMA, "session_id": session_id,
        "state": "collecting", "accepted_pairs": 0, "rejected_pairs": 0,
        "required_pairs": configuration["thresholds"]["minimum_pairs"],
        "coverage": {"score": 0.0, "occupied_cells": 0, "total_cells": 9,
                     "size_bands": []},
        "captures": [], "validation": None, "bundle_id": None,
        "active": False, "created_unix_ns": time.time_ns(),
        "capture_attempts": 0, "attempted_capture_ids": [], "storage_reserved_bytes": 0,
        "limits": {"maximum_capture_attempts": MAX_SESSION_CAPTURES,
                   "maximum_session_bytes": MAX_SESSION_BYTES},
        "message": "Position the measured ChArUco target and capture varied poses.",
    }
    with exclusive_lock(state_root / ".calibration-sessions.lock"):
        _cleanup_sessions_locked(state_root, needed_bytes=base_budget, need_slot=True)
        retained = [path for path in (state_root / "calibration-sessions").glob("*")
                    if path.is_dir() and SESSION_ID.fullmatch(path.name)]
        if len(retained) >= MAX_RETAINED_SESSIONS:
            raise ValueError("calibration retained session limit reached; finish or allow idle sessions to expire")
        if sum(_session_charge(path) for path in retained) + base_budget > MAX_SESSION_STORE_BYTES:
            raise ValueError("calibration session store storage quota reached")
        directory.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".session-staging-", dir=directory.parent))
        try:
            os.chmod(staging, 0o750)
            (staging / "captures").mkdir(mode=0o770)
            (staging / ".operation.lock").touch(mode=0o600)
            atomic_json(staging / "configuration.json", configuration)
            _save_session(staging, status)
            fsync_directory(staging)
            os.replace(staging, directory)
            fsync_directory(directory.parent)
        finally:
            if staging.exists():
                shutil.rmtree(staging)
    return status


def read_web_status(state_root: Path, session_id: str) -> dict:
    directory = _session_path(state_root, session_id)
    status = _session_status(directory)
    try:
        with try_lock(directory / ".operation.lock") as acquired:
            if acquired:
                status = _recover_session(directory)
    except FileNotFoundError:
        pass
    active = state_root / "calibrations" / "user" / "active"
    factory = state_root / "calibrations" / "factory" / "active" / "manifest.json"
    selected = (active.is_symlink() and os.readlink(active) == status.get("bundle_id")
                and not factory.is_file())
    if status.get("state") == "active" and not selected:
        status["state"] = "passed"
        status["message"] = "Calibration passed and is available to activate."
    status["active"] = status.get("state") == "active" and selected
    return status


def _opencv_aruco():
    """Check bindings lazily: recovery and activation do not need OpenCV."""
    import cv2
    version = re.match(r"^(\d+)\.(\d+)", cv2.__version__)
    aruco = getattr(cv2, "aruco", None)
    required = ("getPredefinedDictionary", "CORNER_REFINE_NONE")
    missing = [name for name in required if not hasattr(aruco, name)]
    if not (callable(getattr(aruco, "CharucoBoard_create", None)) or
            callable(getattr(aruco, "CharucoBoard", None))):
        missing.append("CharucoBoard constructor")
    if not (callable(getattr(aruco, "DetectorParameters_create", None)) or
            callable(getattr(aruco, "DetectorParameters", None))):
        missing.append("DetectorParameters constructor")
    modern = all(callable(getattr(aruco, name, None))
                 for name in ("CharucoDetector", "CharucoParameters"))
    legacy = all(callable(getattr(aruco, name, None))
                 for name in ("detectMarkers", "interpolateCornersCharuco"))
    if not modern and not legacy:
        missing.append("ChArUco detection API")
    if (version is None or tuple(map(int, version.groups())) < (4, 6) or missing):
        detail = "; missing " + ", ".join(missing) if missing else ""
        raise RuntimeError(
            f"ChArUco calibration requires OpenCV >= 4.6 with aruco support "
            f"(found {cv2.__version__}{detail}); install python3-opencv >= 4.6 "
            "or an equivalent OpenCV contrib build")
    return aruco


def _charuco_board_corners(board):
    import numpy as np
    getter = getattr(board, "getChessboardCorners", None)
    corners = getter() if callable(getter) else getattr(board, "chessboardCorners", None)
    if corners is None:
        raise RuntimeError("OpenCV ChArUco binding lacks board corner coordinates")
    return np.asarray(corners, np.float32)


def _charuco_board(configuration: dict):
    import numpy as np
    aruco = _opencv_aruco()
    target = configuration["target"]
    if target.get("layout_version", CHARUCO_LAYOUT) != CHARUCO_LAYOUT:
        raise ValueError("unsupported ChArUco layout_version; use the OpenCV 4.6+ printed target")
    dictionary_id = getattr(aruco, target["dictionary"], None)
    if dictionary_id is None:
        raise RuntimeError(f"OpenCV aruco binding lacks dictionary {target['dictionary']}")
    dictionary = aruco.getPredefinedDictionary(dictionary_id)
    width, height = target["squares_x"], target["squares_y"]
    square, marker = float(target["square_m"]), float(target["marker_m"])
    factory = getattr(aruco, "CharucoBoard_create", None)
    if callable(factory):
        board = factory(width, height, square, marker, dictionary)
    else:
        board = aruco.CharucoBoard((width, height), square, marker, dictionary)
    if callable(getattr(board, "setLegacyPattern", None)):
        board.setLegacyPattern(False)
    # Verify both ID coordinates and marker placement, including even-row boards.
    # A distribution may backport APIs without preserving the target layout.
    corners = _charuco_board_corners(board)
    expected_corners = np.asarray([(x * square, y * square, 0.)
                                   for y in range(1, height)
                                   for x in range(1, width)], np.float32)
    points_getter, ids_getter = getattr(board, "getObjPoints", None), getattr(board, "getIds", None)
    points = points_getter() if callable(points_getter) else getattr(board, "objPoints", None)
    identifiers = ids_getter() if callable(ids_getter) else getattr(board, "ids", None)
    offset = (square - marker) / 2
    expected_points = []
    for y in range(height):
        for x in range(width):
            if y % 2 == x % 2:
                continue
            left, top = x * square + offset, y * square + offset
            expected_points.append([(left, top, 0.), (left + marker, top, 0.),
                                    (left + marker, top + marker, 0.), (left, top + marker, 0.)])
    expected_points = np.asarray(expected_points, np.float32)
    points = np.asarray(points, np.float32) if points is not None else np.empty(0)
    identifiers = np.asarray(identifiers).reshape(-1) if identifiers is not None else np.empty(0)
    if (corners.shape != expected_corners.shape or points.shape != expected_points.shape or
            not np.allclose(corners, expected_corners, rtol=1e-5, atol=square * 1e-6) or
            not np.allclose(points, expected_points, rtol=1e-5, atol=square * 1e-6) or
            not np.array_equal(identifiers, np.arange(len(expected_points)))):
        raise RuntimeError("OpenCV ChArUco board layout is incompatible with the OpenCV 4.6+ target")
    return board


def _draw_charuco_board(board, size: tuple[int, int], *, margin_size: int = 0,
                        border_bits: int = 1):
    draw = getattr(board, "generateImage", None)
    if not callable(draw):
        draw = getattr(board, "draw", None)
    if not callable(draw):
        raise RuntimeError("OpenCV ChArUco binding lacks target image generation")
    return draw(size, marginSize=margin_size, borderBits=border_bits)


def _detect_charuco(image, board) -> tuple:
    aruco = _opencv_aruco()
    factory = getattr(aruco, "DetectorParameters_create", None)
    detector_parameters = factory() if callable(factory) else aruco.DetectorParameters()
    # ChArUco interpolation performs its own sub-pixel refinement. Refining the
    # marker corners first can move them inconsistently when no intrinsics are
    # available during capture-time analysis.
    detector_parameters.cornerRefinementMethod = aruco.CORNER_REFINE_NONE
    if (callable(getattr(aruco, "CharucoDetector", None)) and
            callable(getattr(aruco, "CharucoParameters", None))):
        detector = aruco.CharucoDetector(board, aruco.CharucoParameters(), detector_parameters)
        corners, identifiers, marker_corners, marker_identifiers = detector.detectBoard(image)
    else:
        marker_corners, marker_identifiers, _ = aruco.detectMarkers(
            image, board.getDictionary() if callable(getattr(board, "getDictionary", None))
            else board.dictionary, parameters=detector_parameters)
        corners, identifiers = None, None
        if marker_identifiers is not None and len(marker_identifiers):
            _, corners, identifiers = aruco.interpolateCornersCharuco(
                marker_corners, marker_identifiers, image, board, minMarkers=2)
    if corners is None or identifiers is None:
        corners = None
        identifiers = None
    return corners, identifiers, marker_corners, marker_identifiers


def _common_charuco(left_corners, left_ids, right_corners, right_ids, board):
    import numpy as np
    if left_corners is None or right_corners is None:
        return None
    left_lookup = {int(identifier): left_corners[index]
                   for index, identifier in enumerate(left_ids.reshape(-1))}
    right_lookup = {int(identifier): right_corners[index]
                    for index, identifier in enumerate(right_ids.reshape(-1))}
    identifiers = sorted(set(left_lookup).intersection(right_lookup))
    if not identifiers:
        return None
    board_points = _charuco_board_corners(board)
    objects = np.asarray([board_points[index] for index in identifiers], np.float32)
    left = np.asarray([left_lookup[index] for index in identifiers], np.float32).reshape(-1, 1, 2)
    right = np.asarray([right_lookup[index] for index in identifiers], np.float32).reshape(-1, 1, 2)
    return objects, left, right, identifiers


def _capture_metrics(image, corners) -> dict:
    import cv2
    import numpy as np
    height, width = image.shape[:2]
    points = corners.reshape(-1, 2)
    x, y, w, h = cv2.boundingRect(points.astype(np.float32))
    padding = max(2, int(min(width, height) * .01))
    x0, y0 = max(0, x - padding), max(0, y - padding)
    x1, y1 = min(width, x + w + padding), min(height, y + h + padding)
    region = image[y0:y1, x0:x1]
    hull_area = float(cv2.contourArea(cv2.convexHull(points.astype(np.float32))))
    area_fraction = hull_area / float(width * height)
    center = np.mean(points, axis=0)
    rectangle = cv2.minAreaRect(points.astype(np.float32))
    clipped = float(np.mean((region <= 3) | (region >= 252))) if region.size else 1.0
    return {
        "sharpness": float(cv2.Laplacian(region, cv2.CV_64F).var()) if region.size else 0.0,
        "mean_luma": float(np.mean(region)) if region.size else 0.0,
        "clipped_fraction": clipped,
        "board_area_fraction": area_fraction,
        "center_x": float(center[0] / width), "center_y": float(center[1] / height),
        "angle_deg": float(rectangle[2]),
    }


def _coverage(captures: list[dict]) -> dict:
    accepted = [item for item in captures if item.get("accepted")]
    cells = set()
    bands = set()
    for item in accepted:
        metric = item["quality"]["left"]
        cells.add((min(2, int(metric["center_x"] * 3)),
                   min(2, int(metric["center_y"] * 3))))
        area = metric["board_area_fraction"]
        bands.add("near" if area >= .22 else "mid" if area >= .07 else "far")
    cell_score = len(cells) / 9.0
    distance_score = len(bands) / 3.0
    score = .75 * cell_score + .25 * distance_score
    return {"score": round(score, 4), "occupied_cells": len(cells),
            "total_cells": 9, "size_bands": sorted(bands)}


def _is_duplicate_pose(existing: list[dict], metric: dict) -> bool:
    for item in existing:
        if not item.get("accepted"):
            continue
        previous = item["quality"]["left"]
        center_distance = math.hypot(metric["center_x"] - previous["center_x"],
                                     metric["center_y"] - previous["center_y"])
        scale_distance = abs(math.sqrt(metric["board_area_fraction"]) -
                             math.sqrt(previous["board_area_fraction"]))
        angle_distance = abs(metric["angle_deg"] - previous["angle_deg"])
        angle_distance = min(angle_distance, abs(angle_distance - 90.0))
        if center_distance < .045 and scale_distance < .025 and angle_distance < 5.0:
            return True
    return False


def _validate_capture_commit(directory: Path, session_id: str,
                             capture_id: str, configuration: dict) -> dict:
    captures = directory / "captures"
    manifest = json.loads((captures / f"{capture_id}.complete.json").read_text())
    if (manifest.get("schema_version") != 1 or manifest.get("state") != "complete" or
            manifest.get("session_id") != session_id or manifest.get("capture_id") != capture_id):
        raise ValueError("calibration capture commit identity is invalid")
    frame = manifest.get("frame")
    if not isinstance(frame, dict):
        raise ValueError("calibration capture has no coherent frame identity")
    for key in ("device_uuid", "run_uuid"):
        try:
            parsed = uuid.UUID(frame[key])
            if parsed.int == 0 or str(parsed) != frame[key]:
                raise ValueError()
        except (ValueError, KeyError, TypeError, AttributeError) as error:
            raise ValueError(f"calibration capture {key} is invalid") from error
    for key in ("capture_epoch", "capture_sequence", "capture_timestamp_ns"):
        if not isinstance(frame.get(key), int) or isinstance(frame[key], bool) or frame[key] <= 0:
            raise ValueError(f"calibration capture {key} is invalid")
    assets = manifest.get("assets")
    if not isinstance(assets, dict) or set(assets) != {"left", "right", "metadata"}:
        raise ValueError("calibration capture commit assets are incomplete")
    profile = configuration["profile"]
    maximum_image_bytes = (profile["image_width"] * profile["image_height"] * 101 // 100 +
                           profile["image_height"] + 65536)
    for side, suffix in (("left", "-left.png"), ("right", "-right.png"),
                         ("metadata", ".json")):
        asset = assets[side]
        path = captures / f"{capture_id}{suffix}"
        if (not isinstance(asset, dict) or asset.get("name") != path.name or path.is_symlink() or
                not path.is_file() or asset.get("bytes") != path.stat().st_size or
                path.stat().st_size > (maximum_image_bytes if side != "metadata" else 16384) or
                sha256(path) != asset.get("sha256") or asset.get("frame") != frame):
            raise ValueError(f"calibration capture {side} asset integrity is invalid")
        if side != "metadata":
            with path.open("rb") as source:
                header = source.read(24)
            if (len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or
                    header[12:16] != b"IHDR" or
                    int.from_bytes(header[16:20], "big") != profile["image_width"] or
                    int.from_bytes(header[20:24], "big") != profile["image_height"] or
                    asset.get("width") != profile["image_width"] or
                    asset.get("height") != profile["image_height"]):
                raise ValueError("calibration capture PNG dimensions disagree with the profile")
    metadata = json.loads((captures / f"{capture_id}.json").read_text())
    if (metadata.get("schema_version") != 1 or metadata.get("frame") != frame or
            metadata.get("left_camera_id") != configuration["identity"]["left_camera_id"] or
            metadata.get("right_camera_id") != configuration["identity"]["right_camera_id"]):
        raise ValueError("calibration capture metadata disagrees with the committed pair")
    return metadata


def analyze_web_capture(state_root: Path, session_id: str, capture_id: str,
                        wait_seconds: float = 8.0,
                        request_root: Path | None = None) -> dict:
    if not CAPTURE_ID.fullmatch(capture_id):
        raise ValueError("unsafe calibration capture identifier")
    _finite_number(wait_seconds, "capture wait_seconds", 0, 30)
    with _session_operation(state_root, session_id) as directory:
        status = _session_status(directory)
        if any(item["capture_id"] == capture_id for item in status["captures"]):
            return status  # Retry of an already completed analysis is idempotent.
        if status["state"] not in {"collecting", "ready", "failed"}:
            raise ValueError("calibration session is not accepting captures")
        configuration = json.loads((directory / "configuration.json").read_text())
        with exclusive_lock(state_root / ".calibration-sessions.lock"):
            attempted = status.setdefault("attempted_capture_ids",
                                          [item["capture_id"] for item in status["captures"]])
            if capture_id in attempted:
                raise ValueError("calibration capture identifier was already attempted; use a new identifier")
            attempts = status.setdefault("capture_attempts", len(attempted))
            budget = _capture_budget(configuration)
            charge = _session_charge(directory)
            if attempts >= MAX_SESSION_CAPTURES:
                raise ValueError("calibration session capture attempt limit reached")
            if charge + budget > MAX_SESSION_BYTES:
                raise ValueError("calibration session storage quota reached")
            _cleanup_sessions_locked(state_root, needed_bytes=budget)
            retained = [path for path in (state_root / "calibration-sessions").glob("*")
                        if path.is_dir() and SESSION_ID.fullmatch(path.name)]
            if sum(_session_charge(path) for path in retained) + budget > MAX_SESSION_STORE_BYTES:
                raise ValueError("calibration session store storage quota reached")
            attempted.append(capture_id)
            status["capture_attempts"] = attempts + 1
            # Keep the reservation charge even on cancellation: the daemon may
            # already be writing. No late publication can bypass the quota.
            status["storage_reserved_bytes"] = status.get("storage_reserved_bytes", 0) + budget
            status["state"] = "capturing"
            status["pending_capture"] = capture_id
            status["message"] = "Waiting for a committed synchronized camera pair."
            _save_session(directory, status)
        try:
            return _analyze_web_capture(state_root, session_id, capture_id, wait_seconds, request_root)
        except Exception as error:
            _remove_capture(directory, capture_id)
            status = _session_status(directory)
            status.pop("pending_capture", None)
            status["state"] = "failed"
            status["last_error"] = {"operation": "capture", "message": str(error), "interrupted": False}
            status["message"] = f"Capture failed: {error}. Retry with a new capture."
            _save_session(directory, status)
            raise
        finally:
            (directory / "captures" / f"{capture_id}.reservation").unlink(missing_ok=True)
            if request_root is not None:
                request_path = request_root / f"{session_id}--{capture_id}.request"
                request_path.unlink(missing_ok=True)
                request_path.with_name(request_path.name + ".error").unlink(missing_ok=True)


def _analyze_web_capture(state_root: Path, session_id: str, capture_id: str,
                        wait_seconds: float = 8.0,
                        request_root: Path | None = None) -> dict:
    import cv2
    if not CAPTURE_ID.fullmatch(capture_id):
        raise ValueError("unsafe calibration capture identifier")
    directory = _session_path(state_root, session_id)
    configuration = json.loads((directory / "configuration.json").read_text(encoding="utf-8"))
    status = _session_status(directory)
    captures = directory / "captures"
    left_path, right_path = captures / f"{capture_id}-left.png", captures / f"{capture_id}-right.png"
    request_path = None
    error_path = None
    if request_root is not None:
        request_root.mkdir(parents=True, exist_ok=True)
        request_path = request_root / f"{session_id}--{capture_id}.request"
        error_path = request_path.with_name(request_path.name + ".error")
        profile = configuration["profile"]
        maximum_image_bytes = (profile["image_width"] * profile["image_height"] * 101 // 100 +
                               profile["image_height"] + 65536)
        reservation = (f"{uuid.uuid4().hex} {profile['image_width']} {profile['image_height']} "
                       f"{maximum_image_bytes}\n").encode("ascii")
        atomic_bytes(captures / f"{capture_id}.reservation", reservation)
        atomic_bytes(request_path, reservation)
    committed = captures / f"{capture_id}.complete.json"
    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline and not committed.is_file():
        if error_path is not None and error_path.is_file():
            message = error_path.read_text(encoding="utf-8").strip()
            error_path.unlink(missing_ok=True)
            raise RuntimeError(message or "camera daemon rejected the calibration capture")
        time.sleep(.025)
    if not committed.is_file():
        raise RuntimeError("camera daemon did not commit the calibration capture pair")
    camera_metadata = _validate_capture_commit(directory, session_id, capture_id, configuration)
    left = cv2.imread(str(left_path), cv2.IMREAD_GRAYSCALE)
    right = cv2.imread(str(right_path), cv2.IMREAD_GRAYSCALE)
    if left is None or right is None or left.shape != right.shape:
        raise RuntimeError("captured calibration pair is unreadable or mismatched")
    expected = (configuration["profile"]["image_height"],
                configuration["profile"]["image_width"])
    if left.shape != expected:
        raise ValueError(
            f"captured image is {left.shape[1]}x{left.shape[0]}, expected "
            f"{expected[1]}x{expected[0]}")
    board = _charuco_board(configuration)
    lc, li, lm, lmi = _detect_charuco(left, board)
    rc, ri, rm, rmi = _detect_charuco(right, board)
    common = _common_charuco(lc, li, rc, ri, board)
    common_count = 0 if common is None else len(common[3])
    left_metric = _capture_metrics(left, lc) if lc is not None else None
    right_metric = _capture_metrics(right, rc) if rc is not None else None
    reasons: list[str] = []
    if common_count < configuration["thresholds"]["minimum_shared_corners"]:
        reasons.append("not enough ChArUco corners are visible in both cameras")
    if left_metric is not None and right_metric is not None:
        for side, metric in (("left", left_metric), ("right", right_metric)):
            if metric["sharpness"] < 40.0:
                reasons.append(f"{side} target is blurred")
            if metric["clipped_fraction"] > .08 or not 15.0 <= metric["mean_luma"] <= 240.0:
                reasons.append(f"{side} target exposure is unsuitable")
            if not .012 <= metric["board_area_fraction"] <= .82:
                reasons.append(f"{side} target size is unsuitable")
        if _is_duplicate_pose(status["captures"], left_metric):
            reasons.append("pose is too similar to an accepted capture")
    elif not reasons:
        reasons.append("ChArUco target was not detected in both cameras")
    accepted = not reasons
    entry = {
        "capture_id": capture_id, "accepted": accepted,
        "common_corners": common_count, "reasons": reasons,
        "quality": {"left": left_metric, "right": right_metric},
        "captured_unix_ns": time.time_ns(),
    }
    entry["camera"] = camera_metadata
    atomic_json(captures / f"{capture_id}-analysis.json", entry)
    for side, image, corners, identifiers, markers, marker_ids in (
            ("left", left, lc, li, lm, lmi), ("right", right, rc, ri, rm, rmi)):
        annotated = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
        if markers is not None and marker_ids is not None:
            cv2.aruco.drawDetectedMarkers(annotated, markers, marker_ids)
        if corners is not None and identifiers is not None:
            cv2.aruco.drawDetectedCornersCharuco(annotated, corners, identifiers)
        colour = (40, 190, 80) if accepted else (40, 70, 230)
        label = ("ACCEPTED" if accepted else "RETAKE") + f" | shared corners {common_count}"
        cv2.putText(annotated, label, (16, 30), cv2.FONT_HERSHEY_SIMPLEX,
                    .68, colour, 2, cv2.LINE_AA)
        destination = directory / f"preview-{side}.jpg"
        encoded, preview = cv2.imencode(".jpg", annotated, [cv2.IMWRITE_JPEG_QUALITY, 88])
        if not encoded or len(preview) > 3 * annotated.shape[0] * annotated.shape[1] + 65536:
            raise RuntimeError("could not write calibration preview")
        atomic_bytes(destination, preview.tobytes())
    status["captures"].append(entry)
    status["accepted_pairs"] = sum(item["accepted"] for item in status["captures"])
    status["rejected_pairs"] = len(status["captures"]) - status["accepted_pairs"]
    status["coverage"] = _coverage(status["captures"])
    enough = status["accepted_pairs"] >= status["required_pairs"]
    covered = status["coverage"]["score"] >= configuration["thresholds"]["minimum_coverage"]
    status["state"] = "ready" if enough and covered else "collecting"
    status["message"] = ("Capture set is ready to solve." if status["state"] == "ready"
                         else (reasons[0] if reasons else
                               "Accepted. Move the target to a new position, distance, and tilt."))
    status.pop("pending_capture", None)
    status.pop("last_error", None)
    _save_session(directory, status)
    return status


def _calibrate_camera(objects, points, image_size):
    import cv2
    result = cv2.calibrateCameraExtended(
        objects, points, image_size, None, None,
        # The runtime publishes the five-coefficient plumb-bob model. Enabling
        # unconstrained rational terms here makes planar data ill-conditioned
        # and can fit each camera while destroying the stereo solution.
        flags=0,
        criteria=(cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 200, 1e-9))
    return result[0], result[1], result[2], result[3], result[4], result[5], result[7].reshape(-1)


def _rectified_vertical_errors(left_points, right_points, kl, dl, kr, dr, rl, rr, pl, pr):
    import cv2
    import numpy as np
    values = []
    per_view = []
    for left, right in zip(left_points, right_points):
        l = cv2.undistortPoints(left, kl, dl, R=rl, P=pl)
        r = cv2.undistortPoints(right, kr, dr, R=rr, P=pr)
        error = np.abs(l[:, 0, 1] - r[:, 0, 1])
        values.extend(error.tolist())
        per_view.append(float(np.sqrt(np.mean(np.square(error)))))
    return np.asarray(values, dtype=np.float64), per_view


def solve_web_session(state_root: Path, session_id: str) -> dict:
    with _session_operation(state_root, session_id) as directory:
        status = _session_status(directory)
        if status["state"] not in {"collecting", "ready", "failed", "passed"}:
            raise ValueError("calibration session is not available to solve")
        try:
            return _solve_web_session(state_root, session_id)
        except Exception as error:
            status = _session_status(directory)
            status["state"] = "failed"
            status["active"] = False
            status["last_error"] = {"operation": "solve", "message": str(error), "interrupted": False}
            status["message"] = f"Calibration solve failed: {error}. Review captures and retry."
            _save_session(directory, status)
            raise
        finally:
            for temporary in directory.glob(".bundle-staging-*"):
                if temporary.is_dir() and not temporary.is_symlink():
                    shutil.rmtree(temporary)


def _solve_web_session(state_root: Path, session_id: str) -> dict:
    import cv2
    import numpy as np
    import yaml
    directory = _session_path(state_root, session_id)
    configuration = json.loads((directory / "configuration.json").read_text(encoding="utf-8"))
    status = _session_status(directory)
    if status["accepted_pairs"] < configuration["thresholds"]["minimum_pairs"]:
        raise ValueError("not enough accepted calibration pairs")
    if status["coverage"]["score"] < configuration["thresholds"]["minimum_coverage"]:
        raise ValueError("capture coverage is incomplete")
    status["state"] = "solving"
    status["message"] = "Solving intrinsics, stereo geometry, and holdout error."
    _save_session(directory, status)
    board = _charuco_board(configuration)
    accepted = [item for item in status["captures"] if item["accepted"]]
    objects, left_points, right_points, capture_ids = [], [], [], []
    image_size = None
    for item in accepted:
        capture_id = item["capture_id"]
        _validate_capture_commit(directory, session_id, capture_id, configuration)
        left = cv2.imread(str(directory / "captures" / f"{capture_id}-left.png"), cv2.IMREAD_GRAYSCALE)
        right = cv2.imread(str(directory / "captures" / f"{capture_id}-right.png"), cv2.IMREAD_GRAYSCALE)
        lc, li, _, _ = _detect_charuco(left, board)
        rc, ri, _, _ = _detect_charuco(right, board)
        common = _common_charuco(lc, li, rc, ri, board)
        if common is None:
            continue
        obj, lp, rp, _ = common
        objects.append(obj); left_points.append(lp); right_points.append(rp)
        capture_ids.append(capture_id)
        image_size = (left.shape[1], left.shape[0])
    minimum = configuration["thresholds"]["minimum_pairs"]
    if len(objects) < minimum:
        raise RuntimeError("accepted target detections changed before solve")
    holdout_indices = [index for index in range(len(objects)) if index % 5 == 4]
    train_indices = [index for index in range(len(objects)) if index not in holdout_indices]
    if len(train_indices) < 10 or len(holdout_indices) < 2:
        train_indices = list(range(len(objects)))
        holdout_indices = []

    def select(values, indices): return [values[index] for index in indices]
    train_objects = select(objects, train_indices)
    train_left = select(left_points, train_indices)
    train_right = select(right_points, train_indices)
    left_rms, kl, dl, _, _, std_kl, left_view_error = _calibrate_camera(
        train_objects, train_left, image_size)
    right_rms, kr, dr, _, _, std_kr, right_view_error = _calibrate_camera(
        train_objects, train_right, image_size)
    combined = np.maximum(left_view_error, right_view_error)
    median = float(np.median(combined))
    mad = float(np.median(np.abs(combined - median)))
    cutoff = median + max(.05, 3.5 * 1.4826 * mad)
    keep_local = [index for index, error in enumerate(combined) if error <= cutoff]
    rejected_ids = [capture_ids[train_indices[index]] for index, error in enumerate(combined)
                    if error > cutoff]
    if len(keep_local) >= 10 and len(keep_local) < len(train_objects):
        train_objects = select(train_objects, keep_local)
        train_left = select(train_left, keep_local)
        train_right = select(train_right, keep_local)
        left_rms, kl, dl, _, _, std_kl, left_view_error = _calibrate_camera(
            train_objects, train_left, image_size)
        right_rms, kr, dr, _, _, std_kr, right_view_error = _calibrate_camera(
            train_objects, train_right, image_size)
    stereo_rms, kl, dl, kr, dr, rotation, translation, _, _ = cv2.stereoCalibrate(
        train_objects, train_left, train_right, kl, dl, kr, dr, image_size,
        flags=cv2.CALIB_FIX_INTRINSIC,
        criteria=(cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 300, 1e-9))
    rl, rr, pl, pr, q, roi_l, roi_r = cv2.stereoRectify(
        kl, dl, kr, dr, image_size, rotation, translation,
        flags=cv2.CALIB_ZERO_DISPARITY, alpha=0)
    train_errors, train_per_view = _rectified_vertical_errors(
        train_left, train_right, kl, dl, kr, dr, rl, rr, pl, pr)
    holdout_errors, holdout_per_view = _rectified_vertical_errors(
        select(left_points, holdout_indices), select(right_points, holdout_indices),
        kl, dl, kr, dr, rl, rr, pl, pr) if holdout_indices else (np.array([]), [])
    validation_errors = holdout_errors if holdout_errors.size else train_errors
    baseline = float(np.linalg.norm(translation))
    baseline_error = abs(baseline - configuration["profile"]["baseline_m"])
    thresholds = configuration["thresholds"]
    p50 = float(np.percentile(validation_errors, 50))
    p95 = float(np.percentile(validation_errors, 95))
    maximum = float(np.max(validation_errors))
    passed = (left_rms <= thresholds["maximum_rms_px"] and
              right_rms <= thresholds["maximum_rms_px"] and
              stereo_rms <= thresholds["maximum_rms_px"] and
              p95 <= thresholds["maximum_epipolar_p95_px"] and
              baseline_error <= thresholds["baseline_tolerance_m"] and
              status["coverage"]["score"] >= thresholds["minimum_coverage"])
    authority = "user_validated" if passed else "provisional_recalibration_required"
    bundle = Path(tempfile.mkdtemp(prefix=".bundle-staging-", dir=directory))
    os.chmod(bundle, 0o750)
    common_document = {
        "calibration_status": authority, "baseline_m": baseline,
        "image_width": image_size[0], "image_height": image_size[1],
        "distortion_model": "plumb_bob",
    }
    for name, camera_name, k, d, r, p in (
            ("front_left.yaml", "front_left", kl, dl, rl, pl),
            ("front_right.yaml", "front_right", kr, dr, rr, pr)):
        document = {**common_document, "camera_name": camera_name,
                    "camera_matrix": matrix_yaml("K", k),
                    "distortion_coefficients": matrix_yaml("D", d.reshape(1, -1)),
                    "rectification_matrix": matrix_yaml("R", r),
                    "projection_matrix": matrix_yaml("P", p)}
        with (bundle / name).open("x", encoding="utf-8") as target:
            yaml.safe_dump(document, target, sort_keys=False)
    validation = {
        "schema_version": SCHEMA, "status": "pass" if passed else "fail",
        "accepted_pairs": len(objects), "training_pairs": len(train_objects),
        "holdout_pairs": len(holdout_indices), "outlier_capture_ids": rejected_ids,
        "left_rms_px": float(left_rms), "right_rms_px": float(right_rms),
        "stereo_rms_px": float(stereo_rms),
        "rectified_vertical_error_p50_px": p50,
        "rectified_vertical_error_p95_px": p95,
        "rectified_vertical_error_max_px": maximum,
        "baseline_m": baseline, "measured_baseline_m": configuration["profile"]["baseline_m"],
        "baseline_absolute_error_m": baseline_error,
        "coverage": status["coverage"],
        "intrinsic_standard_deviation": {
            "left": [float(value) for value in std_kl.reshape(-1)[:18]],
            "right": [float(value) for value in std_kr.reshape(-1)[:18]],
        },
        "per_view": {"training_epipolar_rms_px": train_per_view,
                     "holdout_epipolar_rms_px": holdout_per_view},
        "thresholds": thresholds,
        "accuracy_note": (("Geometric fit and baseline scale passed. Validate absolute depth "
                           "against known distances before a factory release.") if passed else
                          ("Geometric fit or baseline scale failed its configured gate. This "
                           "bundle cannot be activated; recapture the target set.")),
    }
    atomic_json(bundle / "validation.json", validation)
    assets = {name: sha256(bundle / name) for name in REQUIRED_ASSETS}
    profile = configuration["profile"]
    identity = configuration["identity"]
    manifest = {
        "schema_version": SCHEMA, **identity, "preset_id": profile["preset_id"],
        "authority": authority, "assets": assets,
        "profile": {"sensor_mode": profile["sensor_mode"],
                    "image_width": image_size[0], "image_height": image_size[1],
                    "frame_rates_hz": sorted(float(rate) for rate in profile["frame_rates_hz"])},
        "provenance": {"solver": "opencv-charuco-robust-holdout",
                       "opencv_version": cv2.__version__, "created_unix_ns": time.time_ns(),
                       "target": {**configuration["target"], "layout_version": CHARUCO_LAYOUT}},
    }
    bundle_id = hashlib.sha256(json.dumps(
        manifest, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    manifest["bundle_id"] = bundle_id
    atomic_json(bundle / "manifest.json", manifest)
    for asset in REQUIRED_ASSETS:
        with (bundle / asset).open("rb") as source:
            os.fsync(source.fileno())
    fsync_directory(bundle)
    destination = directory / "bundle"
    previous = directory / f".bundle-previous-{uuid.uuid4().hex}"
    if destination.exists():
        os.replace(destination, previous)
    try:
        os.replace(bundle, destination)
        fsync_directory(directory)
    except Exception:
        if previous.exists():
            os.replace(previous, destination)
        raise
    finally:
        if previous.exists():
            shutil.rmtree(previous)
    status["validation"] = validation
    status["bundle_id"] = bundle_id
    status["state"] = "passed" if passed else "failed"
    status["message"] = ("Calibration passed and is ready to save or activate." if passed else
                         "Calibration failed one or more quality gates; review the metrics and recapture.")
    status.pop("last_error", None)
    _save_session(directory, status)
    return status


def restart_depth_service(expected_bundle: str | None,
                          status_path: Path = Path("/run/nuvo-depth/status.json")) -> None:
    """Wait for Type=notify readiness, then confirm the newly loaded geometry."""
    old_run = None
    try:
        old_run = json.loads(status_path.read_text(encoding="utf-8")).get("run_uuid")
    except (OSError, ValueError):
        pass
    restarted = subprocess.run(
        ["/usr/bin/systemctl", "restart", "nuvo-depthd-native.service"],
        capture_output=True, text=True, timeout=65, check=False)
    if restarted.returncode != 0:
        raise RuntimeError(restarted.stderr.strip() or "depth service restart failed")
    active = subprocess.run(
        ["/usr/bin/systemctl", "is-active", "--quiet", "nuvo-depthd-native.service"],
        capture_output=True, text=True, timeout=5, check=False)
    if active.returncode != 0:
        raise RuntimeError("depth service did not remain active after readiness")
    try:
        status = json.loads(status_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise RuntimeError("depth service readiness status is unavailable") from error
    if (status.get("status") != "running" or not status.get("run_uuid") or
            status.get("run_uuid") == old_run):
        raise RuntimeError("depth service did not publish a new running frame")
    if expected_bundle is not None:
        if status.get("calibration_sha256") != expected_bundle:
            raise RuntimeError("depth service loaded a different calibration bundle")
    elif status.get("calibration_authority") != 0:
        raise RuntimeError("depth service did not restore the provisional seed")


def activate_web_session(state_root: Path, session_id: str, *,
                         restart_depth: bool = False) -> dict:
    with _session_operation(state_root, session_id) as directory:
        status = _session_status(directory)
        if status.get("state") not in {"passed", "active"}:
            raise ValueError("only a passing calibration can be activated")
        manifest = validate_bundle(directory / "bundle")
        if manifest["bundle_id"] != status.get("bundle_id"):
            raise ValueError("session status disagrees with its solved calibration bundle")
        status["state"] = "activating"
        status["message"] = "Selecting calibration and waiting for depth service readiness."
        _save_session(directory, status)
        try:
            return _activate_web_session(state_root, session_id, restart_depth=restart_depth)
        except Exception as error:
            status = _session_status(directory)
            status["state"] = "passed"
            status["active"] = False
            status["last_error"] = {"operation": "activate", "message": str(error), "interrupted": False}
            status["message"] = f"Calibration activation failed: {error}. The passing bundle remains available."
            _save_session(directory, status)
            raise


def _activate_web_session(state_root: Path, session_id: str, *,
                          restart_depth: bool = False) -> dict:
    directory = _session_path(state_root, session_id)
    status = _session_status(directory)
    if status.get("state") not in {"passed", "active", "activating"}:
        raise ValueError("only a passing calibration can be activated")
    bundle_id = CalibrationStore(state_root).activate(
        directory / "bundle", "user",
        restart=restart_depth_service if restart_depth else None)
    status["state"] = "active" if restart_depth else "passed"
    status["active"] = restart_depth
    status["bundle_id"] = bundle_id
    status["message"] = ("Calibration preset is active and depth processing loaded it."
                         if restart_depth else
                         "Calibration preset is selected. Restart depth processing to load it.")
    status.pop("last_error", None)
    _save_session(directory, status)
    return status


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="Nuvo stereo calibration workflow")
    commands = root.add_subparsers(dest="command", required=True)
    make = commands.add_parser("solve")
    make.add_argument("--left", required=True); make.add_argument("--right", required=True)
    make.add_argument("--columns", type=int, default=9); make.add_argument("--rows", type=int, default=6)
    make.add_argument("--square-m", type=float, required=True); make.add_argument("--minimum-pairs", type=int, default=20)
    make.add_argument("--maximum-rms", type=float, default=.5); make.add_argument("--maximum-epipolar-p95", type=float, default=.5)
    make.add_argument("--unit-id", required=True); make.add_argument("--left-camera-id", required=True)
    make.add_argument("--right-camera-id", required=True); make.add_argument("--preset-id", required=True)
    make.add_argument("--sensor-mode", default="unspecified")
    make.add_argument("--expected-width", type=int); make.add_argument("--expected-height", type=int)
    make.add_argument("--frame-rates-hz", nargs="+", type=float, required=True)
    make.add_argument("--output", required=True)
    matrix = commands.add_parser("matrix", help="solve every geometry in a reproducible JSON profile plan")
    matrix.add_argument("plan"); matrix.add_argument("--output-root", required=True)
    check = commands.add_parser("validate"); check.add_argument("bundle"); check.add_argument("--factory-trust")
    activate = commands.add_parser("activate"); activate.add_argument("bundle"); activate.add_argument("--state-root",default="/var/lib/nuvo-depth"); activate.add_argument("--namespace",choices=["user","factory"],default="user"); activate.add_argument("--factory-trust",default="/etc/nuvo-depth/factory-trust.json")
    rollback = commands.add_parser("rollback"); rollback.add_argument("--state-root",default="/var/lib/nuvo-depth"); rollback.add_argument("--namespace",choices=["user","factory"],default="user"); rollback.add_argument("--factory-trust",default="/etc/nuvo-depth/factory-trust.json")
    recover = commands.add_parser("recover", help="restore interrupted selections before daemon startup")
    recover.add_argument("--state-root", default="/var/lib/nuvo-depth")
    recover.add_argument("--factory-trust", default="/etc/nuvo-depth/factory-trust.json")
    cleanup = commands.add_parser("web-cleanup", help="remove expired idle calibration evidence")
    cleanup.add_argument("--state-root", default="/var/lib/nuvo-depth")
    web_create = commands.add_parser("web-create", help="create a validated browser calibration session")
    web_create.add_argument("--state-root", default="/var/lib/nuvo-depth")
    web_create.add_argument("--configuration-json", required=True)
    web_status = commands.add_parser("web-status", help="read browser calibration session status")
    web_status.add_argument("--state-root", default="/var/lib/nuvo-depth")
    web_status.add_argument("--session", required=True)
    web_capture = commands.add_parser("web-capture", help="analyze a daemon-captured raw pair")
    web_capture.add_argument("--state-root", default="/var/lib/nuvo-depth")
    web_capture.add_argument("--session", required=True)
    web_capture.add_argument("--capture", required=True)
    web_capture.add_argument("--wait-seconds", type=float, default=8.0)
    web_capture.add_argument("--request-root")
    web_solve = commands.add_parser("web-solve", help="solve and qualify a browser calibration session")
    web_solve.add_argument("--state-root", default="/var/lib/nuvo-depth")
    web_solve.add_argument("--session", required=True)
    web_activate = commands.add_parser("web-activate", help="activate a passing browser calibration")
    web_activate.add_argument("--state-root", default="/var/lib/nuvo-depth")
    web_activate.add_argument("--session", required=True)
    web_activate.add_argument("--restart-depth-service", action="store_true")
    web_rollback = commands.add_parser("web-rollback", help="roll back the active user calibration")
    web_rollback.add_argument("--state-root", default="/var/lib/nuvo-depth")
    web_rollback.add_argument("--restart-depth-service", action="store_true")
    return root


def main() -> None:
    args = parser().parse_args()
    if args.command == "solve": print(solve(args))
    elif args.command == "matrix": print(solve_matrix(args))
    elif args.command == "validate": print(validate_bundle(Path(args.bundle), trusted_keys=Path(args.factory_trust) if args.factory_trust else None)["bundle_id"])
    elif args.command == "activate": print(CalibrationStore(Path(args.state_root), Path(args.factory_trust)).activate(Path(args.bundle), args.namespace))
    elif args.command == "rollback": print(CalibrationStore(Path(args.state_root), Path(args.factory_trust)).rollback(args.namespace))
    elif args.command == "recover": print(json.dumps({"recovered": CalibrationStore(
        Path(args.state_root), Path(args.factory_trust)).recover_before_start()}))
    elif args.command == "web-cleanup": print(json.dumps({"removed_sessions": cleanup_web_sessions(Path(args.state_root))}))
    elif args.command == "web-create": print(json.dumps(create_web_session(
        Path(args.state_root), json.loads(args.configuration_json)), separators=(",", ":")))
    elif args.command == "web-status": print(json.dumps(read_web_status(
        Path(args.state_root), args.session), separators=(",", ":")))
    elif args.command == "web-capture": print(json.dumps(analyze_web_capture(
        Path(args.state_root), args.session, args.capture, args.wait_seconds,
        Path(args.request_root) if args.request_root else None), separators=(",", ":")))
    elif args.command == "web-solve": print(json.dumps(solve_web_session(
        Path(args.state_root), args.session), separators=(",", ":")))
    elif args.command == "web-activate": print(json.dumps(activate_web_session(
        Path(args.state_root), args.session, restart_depth=args.restart_depth_service),
        separators=(",", ":")))
    elif args.command == "web-rollback": print(json.dumps({
        "active_bundle_id": CalibrationStore(Path(args.state_root)).rollback(
            "user", restart=restart_depth_service if args.restart_depth_service else None),
        "state": "rolled_back"}, separators=(",", ":")))


if __name__ == "__main__": main()
