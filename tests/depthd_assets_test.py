#!/usr/bin/env python3
"""Regression tests for non-destructive Depth Daemon asset provisioning."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "deployment" / "depthd_assets.py"
REPOSITORY = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location("depthd_assets", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
depthd_assets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(depthd_assets)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class DepthdAssetsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        probe_target = self.base / "probe-target"
        probe_target.mkdir()
        probe_link = self.base / "probe-link"
        try:
            os.symlink("probe-target", probe_link, target_is_directory=True)
            probe_link.unlink()
        except OSError as exc:
            self.temporary.cleanup()
            self.skipTest(f"directory symlinks are unavailable: {exc}")
        self.artifacts = self.base / "artifacts"
        self.package = self.base / "package"
        self.root = self.base / "root"
        (self.artifacts / "models").mkdir(parents=True)
        (self.package / "config").mkdir(parents=True)
        self.model_data = b"verified-model-v1"
        self.config_data = b"frozen-profile-v1\n"
        (self.artifacts / "models" / "model.hef").write_bytes(self.model_data)
        (self.package / "config" / "profile.yaml").write_bytes(self.config_data)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def manifest(self, release_id: str = "test-v1") -> dict:
        return {
            "schema_version": 1,
            "release_id": release_id,
            "files": [
                {
                    "source_group": "artifact",
                    "source": "models/model.hef",
                    "destination": "models/model.hef",
                    "bytes": len(self.model_data),
                    "sha256": sha256(self.model_data),
                },
                {
                    "source_group": "package",
                    "source": "config/profile.yaml",
                    "destination": "config/profile.yaml",
                    "bytes": len(self.config_data),
                    "sha256": sha256(self.config_data),
                },
            ],
        }

    def install(self, manifest: dict) -> Path:
        return depthd_assets.install(
            manifest, self.artifacts, self.package, self.root, True
        )

    def test_install_verify_and_idempotent_reinstall(self) -> None:
        manifest = self.manifest()
        release = self.install(manifest)
        self.assertEqual(depthd_assets.active_release(self.root), release.resolve())
        installed = depthd_assets.verify_release(release, manifest)
        self.assertEqual(installed["release_id"], "test-v1")
        self.assertEqual(release.stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.install(manifest), release)
        self.assertEqual(os.readlink(self.root / "current"), "releases/test-v1")
        self.assertFalse((self.root / "previous").exists())

    def test_bad_source_fails_before_destination_mutation(self) -> None:
        manifest = self.manifest()
        (self.artifacts / "models" / "model.hef").write_bytes(b"tampered")
        with self.assertRaises(depthd_assets.AssetError):
            self.install(manifest)
        self.assertFalse(self.root.exists())

    def test_installed_tamper_is_detected_and_not_overwritten(self) -> None:
        manifest = self.manifest()
        release = self.install(manifest)
        model = release / "models" / "model.hef"
        model.chmod(0o644)
        model.write_bytes(b"tampered")
        with self.assertRaises(depthd_assets.AssetError):
            depthd_assets.verify_release(release)
        with self.assertRaises(depthd_assets.AssetError):
            self.install(manifest)
        self.assertEqual(model.read_bytes(), b"tampered")

    def test_second_release_and_rollback_swap_links(self) -> None:
        first = self.install(self.manifest("test-v1"))
        self.model_data = b"verified-model-v2"
        (self.artifacts / "models" / "model.hef").write_bytes(self.model_data)
        second = self.install(self.manifest("test-v2"))
        self.assertEqual(depthd_assets.active_release(self.root), second.resolve())
        self.assertEqual(os.readlink(self.root / "previous"), "releases/test-v1")
        depthd_assets.rollback(self.root)
        self.assertEqual(depthd_assets.active_release(self.root), first.resolve())
        self.assertEqual(os.readlink(self.root / "previous"), "releases/test-v2")
        self.assertTrue(second.is_dir())

    def test_manifest_rejects_traversal_and_duplicate_destinations(self) -> None:
        manifest = self.manifest()
        manifest["files"][0]["destination"] = "../escape"
        path = self.base / "bad.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaises(depthd_assets.AssetError):
            depthd_assets._load_manifest(path)

        manifest = self.manifest()
        manifest["files"][1]["destination"] = "models/model.hef"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaises(depthd_assets.AssetError):
            depthd_assets._load_manifest(path)


class ProductionDeploymentFilesTest(unittest.TestCase):
    def test_package_assets_match_production_manifest(self) -> None:
        manifest = depthd_assets._load_manifest(
            REPOSITORY
            / "deployment"
            / "assets"
            / "nuvo-pi5-depth-100mm-assets.json"
        )
        package_entries = [
            entry for entry in manifest["files"] if entry["source_group"] == "package"
        ]
        self.assertEqual(len(package_entries), 3)
        for entry in package_entries:
            depthd_assets._verify_file(REPOSITORY / entry["source"], entry)

        artifact_entries = {
            entry["destination"]: (entry["bytes"], entry["sha256"])
            for entry in manifest["files"]
            if entry["source_group"] == "artifact"
        }
        self.assertEqual(artifact_entries, {})
        self.assertEqual(manifest["target"]["baseline_m"], 0.0996937500869897)
        self.assertEqual(manifest["target"]["role"], "standalone-depth-camera")

    def test_default_geometry_matches_recovered_beta5_assets(self) -> None:
        folder = REPOSITORY / "config/calibration/nuvo_depth_100mm"
        profile = json.loads((folder / "profile.json").read_text())
        self.assertEqual(profile["source_beta5_bundle_id"],
                         "8adf4c8103f1fb7f2811a7d483669cc99c8c7132902700ebb3fd464d19158cf2")
        for name, asset in profile["assets"].items():
            data = (folder / name).read_bytes()
            self.assertEqual(sha256(data), asset["default_seed_sha256"])
            restored = data.replace(b"calibration_status: provisional_recalibration_required",
                                    b"calibration_status: user_validated")
            self.assertEqual(sha256(restored), asset["beta5_user_validated_sha256"])

    def test_native_production_unit_owns_capture_and_recovers_calibration(self) -> None:
        unit = (REPOSITORY / "deployment/systemd/nuvo-depthd-native.service").read_text(encoding="utf-8")
        target = (REPOSITORY / "deployment/systemd/nuvo-depth.target").read_text(encoding="utf-8")
        config = (REPOSITORY / "config/nuvo_pi5_depth_100mm.yaml").read_text(encoding="utf-8")
        for required in (
            "User=nuvo-depth", "Group=nuvo-video",
            "ExecStartPre=+/usr/bin/systemctl start nuvo-trigger-prepare.service",
            "nuvo-calibration recover --state-root /var/lib/nuvo-depth",
            "ExecStartPre=/usr/lib/nuvo-depth/depthd-config --check-config /etc/nuvo-depth/daemon.yaml",
            "ExecStart=/usr/bin/depthd --run /etc/nuvo-depth/daemon.yaml",
            "--encoding-config /etc/nuvo-depth/encoding.json",
            "--stereo-control /etc/nuvo-depth/stereo.json",
            "--exposure-control /etc/nuvo-depth/exposure.json",
        ):
            self.assertIn(required, unit)
        self.assertIn("Wants=nuvo-depthd-native.service", target)
        self.assertIn("ExecStart=/usr/bin/depthd --run", unit)
        self.assertIn("nuvo_calibration_control.py recover --startup", unit)
        self.assertNotIn("results.ring", unit)
        for required in ("role: standalone_depth_camera", "profile_id: balanced_728x544_10",
                         "baseline_m: 0.0996937500869897", "backend: cpu_opencv", "fallback_backend: egl_gl_dmabuf"):
            self.assertIn(required, config)

        cmake = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        package_dependencies = cmake.split(
            "set(CPACK_DEBIAN_PACKAGE_DEPENDS", maxsplit=1
        )[1].split(")", maxsplit=1)[0]
        self.assertNotIn("hailo", package_dependencies.lower())
        install_rules = cmake.split(
            "set(DEPTHD_LIBEXEC_DIR", maxsplit=1
        )[1].split("if(BUILD_TESTING)", maxsplit=1)[0]
        self.assertNotIn("semantic", install_rules.lower())
        self.assertNotIn("hailo", install_rules.lower())


if __name__ == "__main__":
    unittest.main()
