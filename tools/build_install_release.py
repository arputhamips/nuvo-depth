#!/usr/bin/env python3
"""Build signed GitHub assets from one already-built, qualified Debian cohort."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import zipfile

import nuvo_release as release


def asset_name(name):
    # GitHub normalizes characters such as Debian's '~' during asset upload.
    # Choose that portable spelling BEFORE hashing/signing; control Version is
    # authoritative and keeps its exact Debian value, including '~beta.N'.
    return re.sub(r"[^A-Za-z0-9_.-]", ".", name)


def build(source, packages, output, repository, tag, version, platform, private_key, extras=None, *, profile='appliance'):
    if profile not in release.PROFILE_PACKAGES:
        raise ValueError('unsupported installation profile')
    installer_name = 'install-nuvo-depth-minimal.sh' if profile == 'minimal' else 'install-nuvo-depth.sh'
    install_doc = 'minimal-install.md' if profile == 'minimal' else 'install.md'
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
        if metadata.get("Package") not in release.PROFILE_PACKAGES[profile]:
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
    if profile != 'appliance': manifest['profile'] = profile
    release.validate_manifest(manifest)
    extras = list(extras or [])
    reserved = {f["name"] for f in files} | {"manifest.json", "manifest.sig", "nuvo-install.pyz", installer_name, "release.pub", "LICENSE", "NOTICE", "README.txt"}
    for path in extras:
        if not release.NAME.fullmatch(path.name) or asset_name(path.name) in reserved or path.is_symlink() or not path.is_file() or path.suffix not in {".gz", ".json", ".txt", ".buildinfo", ".log"}:
            raise ValueError("unsafe or duplicate supplementary release asset")
        reserved.add(asset_name(path.name))
    output.mkdir(parents=True, exist_ok=False)
    import shutil
    for path, _ in selected:
        shutil.copyfile(path, output / asset_name(path.name))
    with zipfile.ZipFile(output / "nuvo-install.pyz", "x", compression=zipfile.ZIP_DEFLATED) as archive:
        modules = ("nuvo_minimal_cli.py", "nuvo_release.py", "nuvoctl.py") if profile == 'minimal' else ("nuvo_install.py", "nuvo_setup.py", "nuvo_release.py", "nuvoctl.py")
        for name in modules:
            text = (source / "tools" / name).read_text(encoding="utf-8")
            if name == "nuvo_release.py":
                text = re.sub(r'^DEFAULT_REPOSITORY = .*$', lambda _: "DEFAULT_REPOSITORY = " + repr(repository), text, flags=re.M)
                text = re.sub(r'^DEFAULT_PUBLIC_KEY = .*$', lambda _: "DEFAULT_PUBLIC_KEY = " + repr(public_key), text, flags=re.M)
            archive.writestr(name, text)
        entry = 'from nuvo_minimal_cli import install_main\ninstall_main()\n' if profile == 'minimal' else 'from nuvo_install import main\nmain()\n'
        archive.writestr("__main__.py", entry)
        for name in ("LICENSE", "NOTICE"):
            archive.write(source / name, name)
    shell = (source / installer_name).read_text(encoding="utf-8")
    shell = re.sub(r'^DEFAULT_REPOSITORY="[^"]*"$', lambda _: f'DEFAULT_REPOSITORY="{repository}"', shell, count=1, flags=re.M)
    shell = re.sub(r'^DEFAULT_RELEASE="[^"]*"$', lambda _: f'DEFAULT_RELEASE="{tag}"', shell, count=1, flags=re.M)
    public_der = subprocess.check_output(["openssl", "pkey", "-pubin", "-outform", "DER"], input=public_key.encode("ascii"))
    fingerprint = hashlib.sha256(public_der).hexdigest()
    shell = re.sub(r'^# SHA-256 of DER public key: .*$', lambda _: "# SHA-256 of DER public key: " + fingerprint, shell, count=1, flags=re.M)
    shell, replacements = re.subn(r"^RELEASE_PUBLIC_KEY_PEM='[^']*'", lambda _: "RELEASE_PUBLIC_KEY_PEM='" + public_key.strip() + "'", shell, count=1, flags=re.M)
    if replacements != 1:
        raise ValueError("bootstrap public-key assignment not found")
    (output / installer_name).write_text(shell, encoding="utf-8", newline="\n")
    (output / "release.pub").write_text(public_key, encoding="ascii", newline="\n")
    for name in ("LICENSE", "NOTICE"):
        shutil.copyfile(source / name, output / name)
    (output / "README.txt").write_text(f'''Nuvo Depth {tag} - Raspberry Pi 5 ARM64 installer bundle

Normally, use the one-line install command on the GitHub release page.
The installer selects and verifies every package; do not install them individually.

For a cached/offline bundle, extract it and run:
  sudo bash ./{installer_name} --bundle "$PWD"
OS dependencies still require the configured APT sources or a local APT cache.

LICENSE and NOTICE contain the software terms. Matching Corresponding Source
is nuvo-depth-source.tar.gz when published by the release workflow; build records
and test results are included alongside the signed manifest.
Documentation: https://github.com/{repository}/blob/{tag}/docs/{install_doc}
''', encoding="utf-8")
    manifest["files"] = files
    for name in ("nuvo-install.pyz", installer_name, "release.pub", "LICENSE", "NOTICE", "README.txt"):
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


def publish_bundle(assets, output, private_key):
    """Expose three downloads; retain signed packages/source/evidence inside."""
    import shutil
    manifest = release.validate_manifest(release.load_json(assets / "manifest.json"))
    release.verify_signature(assets / "manifest.json", assets / "manifest.sig", assets / "release.pub")
    for item in manifest["files"]:
        release.check_asset(assets / item["name"], item)
    output.mkdir(parents=True, exist_ok=False)
    minimal = manifest.get('profile') == 'minimal'
    bundle_name = release.MINIMAL_BUNDLE_NAME if minimal else release.BUNDLE_NAME
    installer_name = 'install-nuvo-depth-minimal.sh' if minimal else 'install-nuvo-depth.sh'
    archive = output / bundle_name
    names = sorted([item["name"] for item in manifest["files"]] + ["manifest.json", "manifest.sig"])
    with tarfile.open(archive, "w:gz", format=tarfile.USTAR_FORMAT) as bundle:
        for name in names:
            path = assets / name
            info = tarfile.TarInfo(name)
            info.size, info.mode = path.stat().st_size, 0o644
            with path.open("rb") as data:
                bundle.addfile(info, data)
    if archive.stat().st_size > release.BUNDLE_LIMIT:
        raise ValueError("installer bundle exceeds bootstrap download limit")
    signature = output / (bundle_name + ".sig")
    subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", str(private_key), "-rawin", "-in", str(archive), "-out", str(signature)], check=True)
    release.verify_signature(archive, signature, assets / "release.pub")
    shutil.copyfile(assets / installer_name, output / installer_name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--packages", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--publication-output", type=Path, help="three customer-facing GitHub downloads")
    parser.add_argument("--repository", required=True, help="GitHub owner/repository for this signed release")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", type=Path, required=True)
    parser.add_argument("--private-key", type=Path, required=True, help="offline release secret; never committed")
    parser.add_argument("--extra", type=Path, action="append", default=[], help="supplementary source/build evidence to include in signed manifest")
    parser.add_argument('--profile', choices=sorted(release.PROFILE_PACKAGES), default='appliance')
    args = parser.parse_args()
    build(args.source, args.packages, args.output, args.repository, args.tag, args.version, release.load_json(args.platform), args.private_key, args.extra, profile=args.profile)
    if args.publication_output:
        publish_bundle(args.output, args.publication_output, args.private_key)
    print("Signed install assets ready: " + str(args.output))


if __name__ == "__main__":
    main()
