#!/usr/bin/env python3
"""Build signed GitHub assets from one already-built, qualified Debian cohort."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

import nuvo_release as release


def asset_name(name):
    # GitHub normalizes characters such as Debian's '~' during asset upload.
    # Choose that portable spelling BEFORE hashing/signing; control Version is
    # authoritative and keeps its exact Debian value, including '~beta.N'.
    return re.sub(r"[^A-Za-z0-9_.-]", ".", name)


def build(source, packages, output, repository, tag, version, platform, private_key, extras=None):
    if not release.REPOSITORY.fullmatch(repository) or not release.TAG.fullmatch(tag):
        raise ValueError("supply the final owner/repository and exact release tag")
    public_key = subprocess.check_output(["openssl", "pkey", "-in", str(private_key), "-pubout"], text=True)
    # Ed25519 is mandatory: RSA signatures cannot satisfy the install contract.
    details = subprocess.check_output(["openssl", "pkey", "-pubin", "-text", "-noout"], input=public_key, text=True)
    if "ED25519" not in details.upper():
        raise ValueError("release key must be Ed25519")
    files, selected = [], []
    for path in sorted(packages.glob("*.deb")):
        fields = subprocess.check_output(["dpkg-deb", "--field", str(path), "Package", "Version", "Architecture"], text=True)
        metadata = dict(line.split(": ", 1) for line in fields.strip().splitlines())
        if metadata.get("Package") not in release.RUNTIME_PACKAGES:
            continue
        selected.append((path, metadata))
        with path.open("rb") as package_file:
            digest = hashlib.file_digest(package_file, "sha256").hexdigest()
        files.append({"name": asset_name(path.name), "size": path.stat().st_size, "sha256": digest,
                      "package": metadata["Package"], "version": metadata["Version"], "architecture": metadata["Architecture"]})
    # Validate everything before producing an output directory.
    placeholder = {"name": "nuvo-install.pyz", "size": 1, "sha256": "0"*64}
    manifest = {"schema_version": 1, "format": "nuvo-depth-release", "repository": repository,
                "tag": tag, "version": version, "platform": platform, "files": files + [placeholder]}
    release.validate_manifest(manifest)
    extras = list(extras or [])
    reserved = {f["name"] for f in files} | {"manifest.json", "manifest.sig", "nuvo-install.pyz", "install-nuvo-depth.sh", "release.pub", "LICENSE", "NOTICE"}
    for path in extras:
        if not release.NAME.fullmatch(path.name) or asset_name(path.name) in reserved or path.is_symlink() or not path.is_file() or path.suffix not in {".gz", ".json", ".txt", ".buildinfo", ".log"}:
            raise ValueError("unsafe or duplicate supplementary release asset")
        reserved.add(asset_name(path.name))
    output.mkdir(parents=True, exist_ok=False)
    import shutil
    for path, _ in selected:
        shutil.copyfile(path, output / asset_name(path.name))
    with zipfile.ZipFile(output / "nuvo-install.pyz", "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in ("nuvo_install.py", "nuvo_setup.py", "nuvo_release.py", "nuvoctl.py"):
            text = (source / "tools" / name).read_text(encoding="utf-8")
            if name == "nuvo_release.py":
                text = re.sub(r'^DEFAULT_REPOSITORY = .*$', lambda _: "DEFAULT_REPOSITORY = " + repr(repository), text, flags=re.M)
                text = re.sub(r'^DEFAULT_PUBLIC_KEY = .*$', lambda _: "DEFAULT_PUBLIC_KEY = " + repr(public_key), text, flags=re.M)
            archive.writestr(name, text)
        archive.writestr("__main__.py", "from nuvo_install import main\nmain()\n")
        for name in ("LICENSE", "NOTICE"):
            archive.write(source / name, name)
    shell = (source / "install-nuvo-depth.sh").read_text(encoding="utf-8")
    shell = re.sub(r'^DEFAULT_REPOSITORY="[^"]*"$', lambda _: f'DEFAULT_REPOSITORY="{repository}"', shell, count=1, flags=re.M)
    public_der = subprocess.check_output(["openssl", "pkey", "-pubin", "-outform", "DER"], input=public_key.encode("ascii"))
    fingerprint = hashlib.sha256(public_der).hexdigest()
    shell = re.sub(r'^# SHA-256 of DER public key: .*$', lambda _: "# SHA-256 of DER public key: " + fingerprint, shell, count=1, flags=re.M)
    shell, replacements = re.subn(r"^RELEASE_PUBLIC_KEY_PEM='[^']*'", lambda _: "RELEASE_PUBLIC_KEY_PEM='" + public_key.strip() + "'", shell, count=1, flags=re.M)
    if replacements != 1:
        raise ValueError("bootstrap public-key assignment not found")
    (output / "install-nuvo-depth.sh").write_text(shell, encoding="utf-8", newline="\n")
    (output / "release.pub").write_text(public_key, encoding="ascii", newline="\n")
    for name in ("LICENSE", "NOTICE"):
        shutil.copyfile(source / name, output / name)
    manifest["files"] = files
    for name in ("nuvo-install.pyz", "install-nuvo-depth.sh", "release.pub", "LICENSE", "NOTICE"):
        data = (output / name).read_bytes()
        manifest["files"].append({"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    for path in extras:
        shutil.copyfile(path, output / asset_name(path.name))
        with path.open("rb") as asset:
            digest = hashlib.file_digest(asset, "sha256").hexdigest()
        manifest["files"].append({"name": asset_name(path.name), "size": path.stat().st_size, "sha256": digest})
    release.validate_manifest(manifest)
    (output / "manifest.json").write_text(json.dumps(manifest, sort_keys=True, indent=2)+"\n", encoding="utf-8")
    subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", str(private_key), "-rawin", "-in", str(output/"manifest.json"), "-out", str(output/"manifest.sig")], check=True)
    release.verify_signature(output/"manifest.json", output/"manifest.sig", output/"release.pub")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--packages", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repository", required=True, help="GitHub owner/repository for this signed release")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", type=Path, required=True)
    parser.add_argument("--private-key", type=Path, required=True, help="offline release secret; never committed")
    parser.add_argument("--extra", type=Path, action="append", default=[], help="supplementary source/build evidence to include in signed manifest")
    args = parser.parse_args()
    build(args.source, args.packages, args.output, args.repository, args.tag, args.version, release.load_json(args.platform), args.private_key, args.extra)
    print("Signed install assets ready: " + str(args.output))


if __name__ == "__main__":
    main()
