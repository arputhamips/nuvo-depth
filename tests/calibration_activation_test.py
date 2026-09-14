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
