"""Strict, signed GitHub Release contract shared by installer and packaging tools."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import urllib.error
import urllib.request

# Public source and signed release assets; forks can override --repository.
DEFAULT_REPOSITORY = "arputhamips/nuvo-depth"
# Matches deployment/release-signing.pub. Private key never belongs in source.
DEFAULT_PUBLIC_KEY = "-----BEGIN PUBLIC KEY-----\nMCowBQYDK2VwAyEAQ0zwQ1yw+Tu532eeDs+iBZpSfVBCobIrCbJzlHaGRWk=\n-----END PUBLIC KEY-----\n"
RUNTIME_PACKAGES = {
    "nuvo-depth", "nuvo-depth-core", "nuvo-depth-platform-pi5",
    "nuvo-depth-gateway", "libnuvo-depth1", "python3-nuvo-depth",
    "nuvo-depth-calibration", "nuvo-depth-tools",
}
REPOSITORY = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
TAG = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.+-]*\Z")
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.+~-]{0,190}\Z")
BUNDLE_NAME = "nuvo-depth-pi5-arm64.tar.gz"
BUNDLE_LIMIT = 256 * 1024**2


def unpack_bundle(archive: Path, destination: Path):
    """Unpack an authenticated, flat release without links or special files."""
    destination.mkdir(parents=True, exist_ok=True)
    names, total = set(), 0
    with tarfile.open(archive, "r:gz") as bundle:
        for member in bundle:
            total += member.size
            if (not member.isfile() or not NAME.fullmatch(member.name) or member.name in names
                    or len(names) >= 66 or member.size > 1024**3 or total > 4 * 1024**3 + 1024**2):
                raise ValueError("unsafe release bundle entry")
            names.add(member.name)
            with bundle.extractfile(member) as source, (destination/member.name).open("xb") as target:
                import shutil
                shutil.copyfileobj(source, target)


def unique_object(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def load_json(path: Path, limit=1024 * 1024):
    if path.stat().st_size > limit:
        raise ValueError("JSON document exceeds its size limit")
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)


def validate_platform(platform):
    if not isinstance(platform, dict) or platform.get("architecture") != "arm64" or platform.get("codename") != "trixie":
        raise ValueError("only the qualified arm64 Trixie platform is supported")
    if not isinstance(platform.get("kernels"), list) or not platform["kernels"] or any(not isinstance(k, str) or not re.fullmatch(r"[A-Za-z0-9.+_\-]{1,128}", k) for k in platform["kernels"]):
        raise ValueError("release must list qualified kernel releases")
    image = platform.get("base_image", {})
    if not isinstance(image, dict) or not image.get("url", "").startswith("https://") or not re.fullmatch(r"[a-f0-9]{64}", image.get("sha256", "")):
        raise ValueError("release must pin the qualified upstream base image URL and SHA-256")
    return platform


def validate_manifest(value, repository=None, tag=None):
    if not isinstance(value, dict) or value.get("schema_version") != 1 or value.get("format") != "nuvo-depth-release":
        raise ValueError("unsupported release manifest")
    if not REPOSITORY.fullmatch(value.get("repository", "")) or not TAG.fullmatch(value.get("tag", "")) or value["tag"] in {"latest", "main", "master"}:
        raise ValueError("release must name a repository and immutable tag")
    if repository and value["repository"] != repository or tag and value["tag"] != tag:
        raise ValueError("manifest does not match requested repository/tag")
    version = value.get("version", "")
    if not isinstance(version, str) or not re.fullmatch(r"[0-9][A-Za-z0-9.+:~\-]{0,100}", version):
        raise ValueError("invalid Debian release version")
    validate_platform(value.get("platform", {}))
    files = value.get("files")
    if not isinstance(files, list) or not 1 <= len(files) <= 64:
        raise ValueError("release file list is invalid")
    names, packages = set(), set()
    for item in files:
        name = item.get("name", "")
        if not NAME.fullmatch(name) or name in names:
            raise ValueError("unsafe/duplicate release asset name")
        names.add(name)
        if not isinstance(item.get("size"), int) or isinstance(item["size"], bool) or not 0 < item["size"] <= 1024**3:
            raise ValueError("invalid release asset size")
        if not re.fullmatch(r"[a-f0-9]{64}", item.get("sha256", "")):
            raise ValueError("invalid release asset checksum")
        if name.endswith(".deb"):
            package = item.get("package")
            if package not in RUNTIME_PACKAGES or package in packages or item.get("version") != version or item.get("architecture") not in {"arm64", "all"}:
                raise ValueError("mixed, duplicate or unexpected runtime package")
            packages.add(package)
    if packages != RUNTIME_PACKAGES or "nuvo-install.pyz" not in names:
        raise ValueError("release is not a complete Nuvo runtime cohort")
    if sum(f["size"] for f in files) > 4 * 1024**3:
        raise ValueError("release exceeds total size bound")
    return value


def verify_signature(manifest: Path, signature: Path, public_key: Path):
    if signature.stat().st_size != 64 or public_key.stat().st_size > 16 * 1024:
        raise ValueError("invalid Ed25519 signature/key size")
    result = subprocess.run(
        ["openssl", "pkeyutl", "-verify", "-pubin", "-inkey", str(public_key),
         "-rawin", "-in", str(manifest), "-sigfile", str(signature)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
    )
    if result.returncode:
        raise ValueError("release manifest signature verification failed")


class HTTPSRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        if not newurl.startswith("https://"):
            raise ValueError("refusing non-HTTPS release redirect")
        return super().redirect_request(request, fp, code, msg, headers, newurl)


def download(url: str, target: Path, limit: int):
    if not url.startswith("https://"):
        raise ValueError("release downloads require HTTPS")
    opener = urllib.request.build_opener(HTTPSRedirect())
    temporary = target.with_name(target.name + ".partial")
    try:
        with opener.open(urllib.request.Request(url, headers={"User-Agent": "Nuvo-Depth-Installer/1"}), timeout=30) as response, temporary.open("xb") as output:
            count = 0
            while chunk := response.read(1024 * 1024):
                count += len(chunk)
                if count > limit:
                    raise ValueError("release download exceeds signed size bound")
                output.write(chunk)
        temporary.replace(target)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise


def check_asset(path: Path, item):
    if path.is_symlink() or not path.is_file() or path.stat().st_size != item["size"]:
        raise ValueError(f"asset size/type mismatch: {item['name']}")
    with path.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if digest != item["sha256"]:
        raise ValueError(f"asset checksum mismatch: {item['name']}")


def acquire_release(destination: Path, public_key: Path, repository=None, tag=None, bundle=None):
    if bundle:
        directory = Path(bundle).resolve()
    else:
        if not repository or not REPOSITORY.fullmatch(repository):
            raise ValueError("configure a GitHub release repository or pass --repository OWNER/REPO")
        if not tag or not TAG.fullmatch(tag) or tag in {"latest", "main", "master"}:
            raise ValueError("pass an exact --release tag (not latest/main)")
        directory = destination
        directory.mkdir(parents=True, exist_ok=True)
        base = f"https://github.com/{repository}/releases/download/{tag}"
        archive = directory / BUNDLE_NAME
        try:
            download(base + "/" + BUNDLE_NAME, archive, BUNDLE_LIMIT)
        except urllib.error.HTTPError as error:
            if error.code != 404:
                raise
            # Preserve installation of older releases with individual assets.
            download(base + "/manifest.json", directory / "manifest.json", 1024 * 1024)
            download(base + "/manifest.sig", directory / "manifest.sig", 64)
        else:
            signature = directory / (BUNDLE_NAME + ".sig")
            download(base + "/" + signature.name, signature, 64)
            verify_signature(archive, signature, public_key)
            unpack_bundle(archive, directory)
            bundle = directory
    verify_signature(directory / "manifest.json", directory / "manifest.sig", public_key)
    manifest = validate_manifest(load_json(directory / "manifest.json"), repository, tag)
    if not bundle:
        for item in manifest["files"]:
            if item["name"].endswith(".deb"):
                download(base + "/" + item["name"], directory / item["name"], item["size"])
    packages = []
    for item in manifest["files"]:
        if not item["name"].endswith(".deb"):
            continue
        path = directory / item["name"]
        check_asset(path, item)
        fields = subprocess.check_output(["dpkg-deb", "--field", str(path), "Package", "Version", "Architecture"], text=True)
        actual = dict(line.split(": ", 1) for line in fields.strip().splitlines())
        if any(actual.get(k) != item[v] for k, v in (("Package", "package"), ("Version", "version"), ("Architecture", "architecture"))):
            raise ValueError("package control metadata does not match signed manifest")
        packages.append(path)
    return manifest, packages
