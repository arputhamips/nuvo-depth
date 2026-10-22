"""Calibration validation and activation transactions; no image-solving imports."""
from __future__ import annotations
import base64,hashlib,json,os,re,shutil,subprocess,tempfile,time
from contextlib import contextmanager
from pathlib import Path
from typing import Callable,Iterator
SCHEMA=1
MAX_PROFILE_RATE_HZ=240.0
REQUIRED_ASSETS=("front_left.yaml","front_right.yaml","validation.json")

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


def provisioning_status(state_root: Path, factory_trust: Path) -> dict:
    """Validate the selected per-unit bundle before installer enables capture.

    Hardware identities/geometry are checked again by the daemon when it opens
    the cameras. This reports bundle acceptance, never manufacturing readiness.
    """
    for namespace in ("factory", "user"):
        active = state_root / "calibrations" / namespace / "active"
        if not active.exists():
            continue
        manifest = validate_bundle(active, require_factory_signature=namespace == "factory", trusted_keys=factory_trust)
        unit_id = (state_root / "identity/device.uuid").read_text().strip()
        if manifest["unit_id"] != unit_id:
            raise ValueError("selected calibration belongs to another device")
        if (namespace == "factory") != (manifest["authority"] == "factory_validated"):
            raise ValueError("selected calibration authority and namespace disagree")
        return {"accepted": True, "authority": manifest["authority"]}
    return {"accepted": False, "authority": "provisional"}


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

