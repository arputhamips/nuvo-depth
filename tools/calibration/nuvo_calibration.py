#!/usr/bin/env python3
"""Guided Nuvo stereo calibration and durable bundle activation."""

from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
import glob
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import time
from typing import Iterator

SCHEMA = 1
REQUIRED_ASSETS = ("front_left.yaml", "front_right.yaml", "validation.json")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try: os.fsync(descriptor)
    finally: os.close(descriptor)


def atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("x", encoding="utf-8") as output:
        json.dump(value, output, sort_keys=True, separators=(",", ":"))
        output.write("\n"); output.flush(); os.fsync(output.fileno())
    os.replace(temporary, path); fsync_directory(path.parent)


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

    def activate(self, source: Path, namespace: str = "user") -> str:
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
            link = active.with_name(".active.tmp")
            try: link.unlink()
            except FileNotFoundError: pass
            os.symlink(bundle_id, link); os.replace(link, active); fsync_directory(active.parent)
            transaction["state"] = "committed"; transaction["committed_unix_ns"] = time.time_ns()
            atomic_json(journal, transaction)
        return bundle_id

    def rollback(self, namespace: str = "user") -> str:
        with exclusive_lock(self.root / ".calibration.lock"):
            journals = sorted(self.transactions.glob("calibration-*.json"), reverse=True)
            for path in journals:
                record = json.loads(path.read_text(encoding="utf-8"))
                previous = record.get("previous")
                if record.get("namespace") == namespace and record.get("state") == "committed" and previous:
                    destination = self.calibrations / namespace / previous
                    validate_bundle(destination,
                                    require_factory_signature=namespace == "factory",
                                    trusted_keys=self.factory_trust)
                    active = self.calibrations / namespace / "active"
                    link = active.with_name(".active.tmp")
                    try: link.unlink()
                    except FileNotFoundError: pass
                    os.symlink(previous, link); os.replace(link, active); fsync_directory(active.parent)
                    atomic_json(self.transactions / f"rollback-{time.time_ns()}.json", {
                        "schema_version":1, "operation":"calibration_rollback",
                        "namespace":namespace, "bundle_id":previous,
                        "state":"committed", "committed_unix_ns":time.time_ns(),
                    })
                    return previous
            raise RuntimeError("no validated previous calibration is available")


def matrix_yaml(name: str, matrix) -> dict:
    import numpy as np
    value = np.asarray(matrix)
    return {"rows": int(value.shape[0]), "cols": int(value.shape[1]),
            "data": [float(x) for x in value.reshape(-1)]}


def solve(args: argparse.Namespace) -> Path:
    import cv2
    import numpy as np
    import yaml
    left_paths, right_paths = sorted(glob.glob(args.left)), sorted(glob.glob(args.right))
    if len(left_paths) != len(right_paths) or len(left_paths) < args.minimum_pairs:
        raise ValueError("left/right calibration sets must have equal counts above the minimum")
    board = (args.columns, args.rows)
    object_points = np.zeros((args.rows * args.columns, 3), np.float32)
    object_points[:, :2] = np.mgrid[0:args.columns, 0:args.rows].T.reshape(-1, 2) * args.square_m
    objects, left_points, right_points = [], [], []
    image_size = None
    for left_path, right_path in zip(left_paths, right_paths):
        left = cv2.imread(left_path, cv2.IMREAD_GRAYSCALE); right = cv2.imread(right_path, cv2.IMREAD_GRAYSCALE)
        if left is None or right is None or left.shape != right.shape:
            continue
        image_size = (left.shape[1], left.shape[0])
        ok_left, corners_left = cv2.findChessboardCornersSB(left, board)
        ok_right, corners_right = cv2.findChessboardCornersSB(right, board)
        if ok_left and ok_right:
            objects.append(object_points.copy()); left_points.append(corners_left); right_points.append(corners_right)
    if len(objects) < args.minimum_pairs or image_size is None:
        raise ValueError("not enough synchronized pairs contain a detectable target")
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
                "provenance":{"solver":"opencv", "opencv_version":cv2.__version__,
                              "created_unix_ns":time.time_ns(), "target":"chessboard",
                              "square_m":args.square_m}}
    identity = hashlib.sha256(json.dumps(manifest,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    manifest["bundle_id"] = identity; atomic_json(output/"manifest.json", manifest)
    if not passed: raise RuntimeError(f"calibration did not meet thresholds; evidence saved at {output}")
    return output


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
    make.add_argument("--output", required=True)
    check = commands.add_parser("validate"); check.add_argument("bundle"); check.add_argument("--factory-trust")
    activate = commands.add_parser("activate"); activate.add_argument("bundle"); activate.add_argument("--state-root",default="/var/lib/nuvo-depth"); activate.add_argument("--namespace",choices=["user","factory"],default="user"); activate.add_argument("--factory-trust",default="/etc/nuvo-depth/factory-trust.json")
    rollback = commands.add_parser("rollback"); rollback.add_argument("--state-root",default="/var/lib/nuvo-depth"); rollback.add_argument("--namespace",choices=["user","factory"],default="user"); rollback.add_argument("--factory-trust",default="/etc/nuvo-depth/factory-trust.json")
    return root


def main() -> None:
    args = parser().parse_args()
    if args.command == "solve": print(solve(args))
    elif args.command == "validate": print(validate_bundle(Path(args.bundle), trusted_keys=Path(args.factory_trust) if args.factory_trust else None)["bundle_id"])
    elif args.command == "activate": print(CalibrationStore(Path(args.state_root), Path(args.factory_trust)).activate(Path(args.bundle), args.namespace))
    elif args.command == "rollback": print(CalibrationStore(Path(args.state_root), Path(args.factory_trust)).rollback(args.namespace))


if __name__ == "__main__": main()
