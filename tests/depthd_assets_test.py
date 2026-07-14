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
            / "nuvo-pi5-hailo8l-assets.json"
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
        self.assertEqual(
            artifact_entries,
            {
                "models/hailo/front_semantic.hef": (
                    28905575,
                    "17270a35bcf3e27a8dcd1dfae8d328a31318bb7c5b7c7a725392b83d008642c6",
                ),
                "models/hailo/segformer_b0_bn.hef": (
                    16738258,
                    "2093951835bafdb0585f93e3eddfb13fac2aa926c45475ccedabe5b2daa7a674",
                ),
                "models/hailo/coco80.labels": (
                    621,
                    "bd17f1ee35d5f3c862a4894605855abbb9dda4b0621fdb0ac4c2c8c7bb7e730a",
                ),
            },
        )

    def test_production_unit_owns_all_runtime_paths_and_locks_profile(self) -> None:
        unit = (
            REPOSITORY / "deployment" / "systemd" / "nuvo-depthd.service"
        ).read_text(encoding="utf-8")
        config = (REPOSITORY / "config" / "nuvo_pi5_hailo8l.yaml").read_text(
            encoding="utf-8"
        )
        profiler = (
            REPOSITORY / "tools" / "pi" / "profile_async_zero_copy_pipeline.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn("/home/cam/nuvo-base", unit + config + profiler)
        for required in (
            "ExecStartPre=/usr/sbin/depthd-assets preflight",
            "--left-camera-index 1",
            "--right-camera-index 0",
            "--capture-stream-profile isp_luma_rgb_left",
            "--fps 10.0",
            "--jpeg-hz 2.0",
            "--ae-max-shutter-us 8000",
            "--ae-max-gain 3.0",
            "--working-width 364",
            "--working-height 272",
            "--working-disparities 64",
            "--full-disparity-limit 128.0",
            "--block-size 5",
            "--uniqueness-ratio 1",
            "--speckle-window-size 30",
            "--result-ring /run/depthd/results.ring",
            "--semantic-ring /run/depthd/semantic.ring",
            "/usr/lib/nuvo-depthd/libdepthd_native_output.so",
            "/usr/lib/nuvo-depthd/libdepthd_native_semantic.so",
            "/var/lib/nuvo-depthd/current/models/hailo/front_semantic.hef",
            "/var/lib/nuvo-depthd/current/models/hailo/segformer_b0_bn.hef",
        ):
            self.assertIn(required, unit)
        self.assertIn("semantic:\n", config)
        self.assertIn("  enabled: true\n", config)
        self.assertIn("  preview:\n    enabled: false\n", config)


if __name__ == "__main__":
    unittest.main()
