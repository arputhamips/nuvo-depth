#!/usr/bin/env python3
"""Provision and verify immutable Depth Daemon runtime-data releases."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any


DEFAULT_MANIFEST = Path(
    "/usr/share/nuvo-depthd/runtime-seed/nuvo-pi5-hailo8l-assets.json"
)
DEFAULT_PACKAGE_SOURCE = Path("/usr/share/nuvo-depthd/runtime-seed")
DEFAULT_ROOT = Path("/var/lib/nuvo-depthd")
INSTALLED_MANIFEST = ".depthd-manifest.json"
_RELEASE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class AssetError(RuntimeError):
    """A manifest, integrity, or activation operation failed safely."""


def _relative_path(value: Any, field: str) -> Path:
    if not isinstance(value, str) or not value:
        raise AssetError(f"{field} must be a non-empty string")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise AssetError(f"{field} must be a normalized relative path: {value!r}")
    return Path(*path.parts)


def _load_manifest(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AssetError(f"cannot read manifest {path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise AssetError("manifest schema_version must be 1")
    release_id = data.get("release_id")
    if not isinstance(release_id, str) or not _RELEASE_RE.fullmatch(release_id):
        raise AssetError("manifest release_id is invalid")
    files = data.get("files")
    if not isinstance(files, list) or not files:
        raise AssetError("manifest files must be a non-empty array")
    destinations: set[Path] = set()
    for index, entry in enumerate(files):
        if not isinstance(entry, dict):
            raise AssetError(f"files[{index}] must be an object")
        group = entry.get("source_group")
        if group not in ("artifact", "package"):
            raise AssetError(f"files[{index}].source_group is invalid")
        _relative_path(entry.get("source"), f"files[{index}].source")
        destination = _relative_path(
            entry.get("destination"), f"files[{index}].destination"
        )
        if destination == Path(INSTALLED_MANIFEST):
            raise AssetError(f"files[{index}] uses a reserved destination")
        if destination in destinations:
            raise AssetError(f"duplicate destination: {destination}")
        destinations.add(destination)
        size = entry.get("bytes")
        digest = entry.get("sha256")
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            raise AssetError(f"files[{index}].bytes must be a non-negative integer")
        if not isinstance(digest, str) or not _SHA256_RE.fullmatch(digest.lower()):
            raise AssetError(f"files[{index}].sha256 must be a SHA-256 digest")
        entry["sha256"] = digest.lower()
    return data


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _verify_file(path: Path, entry: dict[str, Any]) -> None:
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise AssetError(f"missing required asset {path}: {exc}") from exc
    if not path.is_file():
        raise AssetError(f"required asset is not a regular file: {path}")
    if size != entry["bytes"]:
        raise AssetError(
            f"size mismatch for {path}: expected {entry['bytes']}, got {size}"
        )
    digest = _sha256(path)
    if digest != entry["sha256"]:
        raise AssetError(
            f"SHA-256 mismatch for {path}: expected {entry['sha256']}, got {digest}"
        )


def _source_path(
    entry: dict[str, Any], artifact_source: Path, package_source: Path
) -> Path:
    root = artifact_source if entry["source_group"] == "artifact" else package_source
    return root / _relative_path(entry["source"], "source")


def verify_sources(
    manifest: dict[str, Any], artifact_source: Path, package_source: Path
) -> None:
    for entry in manifest["files"]:
        _verify_file(_source_path(entry, artifact_source, package_source), entry)


def _write_json(path: Path, data: dict[str, Any]) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(data, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def _atomic_symlink(link: Path, target: str) -> None:
    temporary = link.parent / f".{link.name}.tmp-{os.getpid()}"
    try:
        temporary.unlink(missing_ok=True)
        os.symlink(target, temporary, target_is_directory=True)
        os.replace(temporary, link)
    finally:
        temporary.unlink(missing_ok=True)


def _read_link(link: Path) -> str | None:
    if link.is_symlink():
        return os.readlink(link)
    if link.exists():
        raise AssetError(f"activation path exists but is not a symlink: {link}")
    return None


def _release_target(root: Path, release_id: str) -> str:
    return (Path("releases") / release_id).as_posix()


def _release_from_target(root: Path, target: str, label: str) -> Path:
    release = (root / target).resolve()
    releases = (root / "releases").resolve()
    if release.parent != releases:
        raise AssetError(f"{label} release escapes {releases}: {release}")
    return release


def activate(root: Path, release_id: str) -> None:
    if not _RELEASE_RE.fullmatch(release_id):
        raise AssetError("release id is invalid")
    release = root / "releases" / release_id
    verify_release(release)
    current = root / "current"
    previous = root / "previous"
    old_target = _read_link(current)
    new_target = _release_target(root, release_id)
    if old_target == new_target:
        return
    if old_target is not None:
        verify_release(_release_from_target(root, old_target, "active"))
        _read_link(previous)
        _atomic_symlink(previous, old_target)
    _atomic_symlink(current, new_target)


def install(
    manifest: dict[str, Any],
    artifact_source: Path,
    package_source: Path,
    root: Path,
    should_activate: bool,
) -> Path:
    # Validate every input before creating or changing the destination root.
    verify_sources(manifest, artifact_source, package_source)
    release_id = manifest["release_id"]
    releases = root / "releases"
    release = releases / release_id
    if release.is_symlink():
        raise AssetError(f"release path must not be a symlink: {release}")
    if release.exists():
        verify_release(release, expected_manifest=manifest)
        if should_activate:
            activate(root, release_id)
        return release

    releases.mkdir(parents=True, exist_ok=True, mode=0o755)
    stage = Path(tempfile.mkdtemp(prefix=f".{release_id}.stage-", dir=releases))
    try:
        for entry in manifest["files"]:
            source = _source_path(entry, artifact_source, package_source)
            destination = stage / _relative_path(entry["destination"], "destination")
            destination.parent.mkdir(parents=True, exist_ok=True, mode=0o755)
            shutil.copyfile(source, destination)
            os.chmod(destination, 0o444)
            _verify_file(destination, entry)
        installed_manifest = dict(manifest)
        installed_manifest["installed_at_utc"] = datetime.now(timezone.utc).isoformat()
        _write_json(stage / INSTALLED_MANIFEST, installed_manifest)
        os.chmod(stage / INSTALLED_MANIFEST, 0o444)
        os.chmod(stage, 0o755)
        os.replace(stage, release)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    verify_release(release, expected_manifest=manifest)
    if should_activate:
        activate(root, release_id)
    return release


def verify_release(
    release: Path, expected_manifest: dict[str, Any] | None = None
) -> dict[str, Any]:
    if release.is_symlink():
        raise AssetError(f"release directory must not be a symlink: {release}")
    if not release.is_dir():
        raise AssetError(f"release directory is missing: {release}")
    installed = _load_manifest(release / INSTALLED_MANIFEST)
    if installed["release_id"] != release.name:
        raise AssetError("installed manifest release_id does not match directory name")
    if expected_manifest is not None:
        for field in ("schema_version", "release_id", "files"):
            if installed.get(field) != expected_manifest.get(field):
                raise AssetError(f"installed release manifest differs in {field}")
    for entry in installed["files"]:
        _verify_file(release / _relative_path(entry["destination"], "destination"), entry)
    return installed


def active_release(root: Path) -> Path:
    current = root / "current"
    target = _read_link(current)
    if target is None:
        raise AssetError(f"no active release: {current} is absent")
    return _release_from_target(root, target, "active")


def rollback(root: Path) -> None:
    current = root / "current"
    previous = root / "previous"
    current_target = _read_link(current)
    previous_target = _read_link(previous)
    if current_target is None or previous_target is None:
        raise AssetError("rollback requires both current and previous releases")
    _release_from_target(root, current_target, "active")
    previous_release = _release_from_target(root, previous_target, "previous")
    verify_release(previous_release)
    _atomic_symlink(current, previous_target)
    _atomic_symlink(previous, current_target)


def _summary(action: str, root: Path, release: Path | None = None) -> None:
    payload: dict[str, Any] = {"action": action, "status": "ok", "root": str(root)}
    if release is not None:
        payload["release"] = release.name
    current = _read_link(root / "current") if root.exists() else None
    previous = _read_link(root / "previous") if root.exists() else None
    payload["current"] = current
    payload["previous"] = previous
    print(json.dumps(payload, sort_keys=True))


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    def source_arguments(command: argparse.ArgumentParser) -> None:
        command.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
        command.add_argument("--artifact-source", type=Path, required=True)
        command.add_argument(
            "--package-source", type=Path, default=DEFAULT_PACKAGE_SOURCE
        )

    source_verify = subparsers.add_parser(
        "verify-source", help="verify a package seed and external artifact directory"
    )
    source_arguments(source_verify)

    install_parser = subparsers.add_parser(
        "install", help="install an immutable release without overwriting existing data"
    )
    source_arguments(install_parser)
    install_parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    install_parser.add_argument(
        "--activate", action=argparse.BooleanOptionalAction, default=True
    )

    verify_parser = subparsers.add_parser("verify", help="verify an installed release")
    verify_parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    verify_parser.add_argument("--release")

    preflight = subparsers.add_parser(
        "preflight", help="verify the active release before daemon startup"
    )
    preflight.add_argument("--root", type=Path, default=DEFAULT_ROOT)

    activate_parser = subparsers.add_parser("activate", help="activate a release")
    activate_parser.add_argument("release")
    activate_parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)

    rollback_parser = subparsers.add_parser(
        "rollback", help="atomically switch current back to previous"
    )
    rollback_parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)

    status = subparsers.add_parser("status", help="show and verify active release")
    status.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        if args.command == "verify-source":
            manifest = _load_manifest(args.manifest)
            verify_sources(manifest, args.artifact_source, args.package_source)
            print(
                json.dumps(
                    {
                        "action": "verify-source",
                        "status": "ok",
                        "release": manifest["release_id"],
                    },
                    sort_keys=True,
                )
            )
        elif args.command == "install":
            manifest = _load_manifest(args.manifest)
            release = install(
                manifest,
                args.artifact_source,
                args.package_source,
                args.root,
                args.activate,
            )
            _summary("install", args.root, release)
        elif args.command == "verify":
            release = (
                args.root / "releases" / args.release
                if args.release
                else active_release(args.root)
            )
            verify_release(release)
            _summary("verify", args.root, release)
        elif args.command in ("preflight", "status"):
            release = active_release(args.root)
            verify_release(release)
            _summary(args.command, args.root, release)
        elif args.command == "activate":
            activate(args.root, args.release)
            _summary("activate", args.root, active_release(args.root))
        elif args.command == "rollback":
            rollback(args.root)
            release = active_release(args.root)
            verify_release(release)
            _summary("rollback", args.root, release)
        else:  # pragma: no cover - argparse enforces the command set.
            raise AssetError(f"unsupported command: {args.command}")
    except AssetError as exc:
        print(f"depthd-assets: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
