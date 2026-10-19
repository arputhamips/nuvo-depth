import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

SOURCE = Path(__file__).parents[1] / "tools/calibration/nuvo_calibration.py"
spec = importlib.util.spec_from_file_location("nuvo_calibration", SOURCE)
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)


def bundle(root: Path, name: str) -> Path:
    result = root / name; result.mkdir()
    for asset in module.REQUIRED_ASSETS:
        value = b'{"status":"pass"}\n' if asset == "validation.json" else name.encode()
        (result / asset).write_bytes(value)
    manifest = {"schema_version":1,"unit_id":"unit-1","left_camera_id":"left",
                "right_camera_id":"right","preset_id":"balanced",
                "authority":"user_validated",
                "assets":{asset:module.sha256(result/asset) for asset in module.REQUIRED_ASSETS}}
    manifest["bundle_id"] = hashlib.sha256(json.dumps(manifest,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    module.atomic_json(result/"manifest.json",manifest)
    return result


def factory_bundle(root: Path, name: str) -> tuple[Path, Path]:
    result = bundle(root, name)
    manifest = json.loads((result / "manifest.json").read_text())
    manifest["authority"] = "factory_validated"
    manifest.pop("bundle_id")
    material = json.dumps(manifest,sort_keys=True,separators=(",",":")).encode()
    manifest["bundle_id"] = hashlib.sha256(material).hexdigest()
    private = Ed25519PrivateKey.generate()
    manifest["signature"] = {
        "algorithm":"ed25519", "key_id":"factory-test",
        "value":base64.b64encode(private.sign(material)).decode(),
    }
    module.atomic_json(result / "manifest.json", manifest)
    public = private.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )
    trust = root / "factory-trust.json"
    module.atomic_json(trust, {"schema_version":1,"keys":{"factory-test":base64.b64encode(public).decode()}})
    return result, trust


class ActivationTest(unittest.TestCase):
    def test_provisioning_status_requires_valid_unit_bound_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            self.assertFalse(module.provisioning_status(root,root/"trust.json")["accepted"])
            source=bundle(root,"bundle")
            store=module.CalibrationStore(root)
            store.activate(source)
            (root/"identity").mkdir()
            (root/"identity/device.uuid").write_text("other-unit")
            with self.assertRaises(ValueError): module.provisioning_status(root,root/"trust.json")
            (root/"identity/device.uuid").write_text("unit-1")
            self.assertTrue(module.provisioning_status(root,root/"trust.json")["accepted"])
            active=root/"calibrations/user/active"
            (active/"validation.json").write_text('{"status":"fail"}')
            with self.assertRaises(ValueError): module.provisioning_status(root,root/"trust.json")
    def test_browser_charuco_session_contract_and_state(self):
        configuration = {
            "schema_version": 1,
            "identity": {"unit_id": "unit-1", "left_camera_id": "platform/csi0/imx296",
                         "right_camera_id": "platform/csi1/imx296"},
            "profile": {"preset_id": "imx296-728x544", "sensor_mode": "imx296-728x544",
                        "image_width": 728, "image_height": 544,
                        "frame_rates_hz": [30, 60], "baseline_m": .1},
            "target": {"type": "charuco", "squares_x": 7, "squares_y": 5,
                       "square_m": .03, "marker_m": .015,
                       "dictionary": "DICT_5X5_100"},
            "thresholds": {"minimum_pairs": 30, "minimum_shared_corners": 12,
                           "maximum_rms_px": .5, "maximum_epipolar_p95_px": .5,
                           "baseline_tolerance_m": .002, "minimum_coverage": .65},
        }
        with tempfile.TemporaryDirectory() as directory:
            status = module.create_web_session(Path(directory), configuration)
            self.assertRegex(status["session_id"], r"^[a-f0-9]{32}$")
            self.assertEqual(status["state"], "collecting")
            self.assertEqual(module.read_web_status(
                Path(directory), status["session_id"])["required_pairs"], 30)
        malformed = json.loads(json.dumps(configuration))
        malformed["target"]["marker_m"] = malformed["target"]["square_m"]
        with self.assertRaisesRegex(ValueError, "marker_m"):
            module.validate_web_configuration(malformed)

    def test_validation_unit_does_not_stop_the_live_depth_service(self):
        unit = (Path(__file__).parents[1] / "deployment/systemd/nuvo-calibration@.service").read_text()
        self.assertNotIn("Conflicts=nuvo-depthd-native.service", unit)
        self.assertIn("nuvo-calibration validate", unit)
        self.assertIn("ReadOnlyPaths=/var/cache/nuvo-depth/calibration-jobs", unit)

    def test_profile_matrix_plan_is_strict_and_reproducible(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "matrix.json"
            plan = {
                "schema_version": 1,
                "unit_id": "unit-1",
                "left_camera_id": "left",
                "right_camera_id": "right",
                "target": {"type": "chessboard", "columns": 9, "rows": 6,
                           "square_m": 0.024},
                "profiles": [
                    {"profile_id": "imx296-1456x1088", "sensor_mode": "1456x1088",
                     "width": 1456, "height": 1088, "frame_rates_hz": [10, 30, 60],
                     "left": "captures/native/left/*.png",
                     "right": "captures/native/right/*.png"},
                    {"profile_id": "imx296-728x544", "sensor_mode": "728x544-binned",
                     "width": 728, "height": 544, "frame_rates_hz": [10, 30, 60],
                     "left": "captures/binned/left/*.png",
                     "right": "captures/binned/right/*.png"},
                ],
            }
            module.atomic_json(path, plan)
            loaded = module.load_matrix_plan(path)
            self.assertEqual([item["profile_id"] for item in loaded["profiles"]],
                             ["imx296-1456x1088", "imx296-728x544"])
            plan["profiles"][1]["profile_id"] = "imx296-1456x1088"
            module.atomic_json(path.with_name("duplicate.json"), plan)
            with self.assertRaisesRegex(ValueError, "duplicate calibration profile"):
                module.load_matrix_plan(path.with_name("duplicate.json"))
            plan["profiles"][1]["profile_id"] = "../escape"
            module.atomic_json(path.with_name("unsafe.json"), plan)
            with self.assertRaisesRegex(ValueError, "unsafe identifier"):
                module.load_matrix_plan(path.with_name("unsafe.json"))

    def test_capture_pair_keys_accept_side_labels_but_not_wrong_indices(self):
        self.assertEqual(module.capture_pair_key("left_000042.png"), "000042")
        self.assertEqual(module.capture_pair_key("right_000042.png"), "000042")
        self.assertEqual(module.capture_pair_key("000042_front-left.png"), "000042")
        self.assertNotEqual(module.capture_pair_key("left_000041.png"),
                            module.capture_pair_key("right_000042.png"))

    def test_profile_metadata_is_validated_before_activation(self):
        with tempfile.TemporaryDirectory() as directory:
            item = bundle(Path(directory), "profile")
            manifest = json.loads((item / "manifest.json").read_text())
            manifest["profile"] = {
                "sensor_mode": "1456x1088", "image_width": 1456,
                "image_height": 1088, "frame_rates_hz": [10, 30, 60],
            }
            manifest.pop("bundle_id")
            manifest["bundle_id"] = hashlib.sha256(json.dumps(
                manifest, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            module.atomic_json(item / "manifest.json", manifest)
            self.assertEqual(module.validate_bundle(item)["profile"]["image_width"], 1456)
            manifest["profile"]["frame_rates_hz"] = []
            manifest.pop("bundle_id")
            manifest["bundle_id"] = hashlib.sha256(json.dumps(
                manifest, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            module.atomic_json(item / "manifest.json", manifest)
            with self.assertRaisesRegex(ValueError, "frame-rate set"):
                module.validate_bundle(item)

    def test_activate_and_rollback(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); store=module.CalibrationStore(root/"state")
            first=bundle(root,"first"); second=bundle(root,"second")
            one=store.activate(first); two=store.activate(second)
            self.assertNotEqual(one,two)
            self.assertEqual((store.calibrations/"user"/"active").resolve().name,two)
            self.assertEqual(store.rollback(),one)
            self.assertEqual((store.calibrations/"user"/"active").resolve().name,one)

    def test_tamper_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            item=bundle(Path(directory),"bundle")
            (item/"front_left.yaml").write_text("tampered")
            self.assertRaises(ValueError,module.validate_bundle,item)

    def test_factory_signature_is_cryptographically_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); item,trust=factory_bundle(root,"factory")
            store=module.CalibrationStore(root/"state",trust)
            bundle_id=store.activate(item,"factory")
            self.assertEqual(bundle_id,module.validate_bundle(item,require_factory_signature=True,trusted_keys=trust)["bundle_id"])
            manifest=json.loads((item/"manifest.json").read_text())
            manifest["signature"]["value"] = base64.b64encode(b"0"*64).decode()
            module.atomic_json(item/"manifest.json",manifest)
            self.assertRaises(ValueError,module.validate_bundle,item,require_factory_signature=True,trusted_keys=trust)


if __name__ == "__main__": unittest.main()
