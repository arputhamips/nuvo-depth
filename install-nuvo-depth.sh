#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Meridian Robotics. See LICENSE for terms and warranty notice.
# The release builder pins DEFAULT_RELEASE in each published installer.
DEFAULT_REPOSITORY="arputhamips/nuvo-depth"
DEFAULT_RELEASE=""
# SHA-256 of DER public key: 6cd440dd00741e095a4dc436001463506eab7e295dcf608a156311e46344ee15
RELEASE_PUBLIC_KEY_PEM='-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAQ0zwQ1yw+Tu532eeDs+iBZpSfVBCobIrCbJzlHaGRWk=
-----END PUBLIC KEY-----'

# Parse the whole function before reattaching a curl pipe to the terminal.
main() {
  set -euo pipefail
  umask 077
  repository="$DEFAULT_REPOSITORY"
  release="$DEFAULT_RELEASE"
  public_key=""
  bundle=""
  factory=false
  arguments=("$@")
  while (($#)); do
    case "$1" in
      --repository|--release|--public-key|--bundle)
        (($# >= 2)) || { echo "Missing value for $1" >&2; exit 2; }
        case "$1" in
          --repository) repository="$2" ;;
          --release) release="$2" ;;
          --public-key) public_key="$2" ;;
          --bundle) bundle="$2" ;;
        esac
        shift 2 ;;
      --factory) factory=true; shift ;;
      --help|-h)
        cat <<'HELP'
Nuvo Depth terminal installer
  --release TAG           Override this installer's pinned release
  --repository OWNER/REPO Override arputhamips/nuvo-depth
  --public-key PATH       Independently trusted Ed25519 release public key
  --factory               Manufacturing preset; no selection prompts
  --wifi-country CC       Actual operating country (factory input if OS unset)
  --reboot                Permit a required reboot
  --dry-run               Inspect without making changes
  --bundle DIRECTORY      Use an extracted signed bundle; APT dependencies remain
Questions and masked passwords use your terminal, including curl | sudo bash.
HELP
        exit 0 ;;
      *) shift ;;
    esac
  done
  if ! "$factory" && [[ ! -t 0 ]]; then
    if ! ( : </dev/tty ) 2>/dev/null; then
      echo 'Normal setup needs a controlling terminal for questions. Use SSH with a terminal or run locally.' >&2
      exit 2
    fi
    exec </dev/tty
  fi
  # A piped script must not trust a tools/ directory in its working directory.
  if [[ "${BASH_SOURCE[0]:-}" == "$0" && -f "$0" ]]; then
    script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
    if [[ -f "$script_dir/tools/nuvo_install.py" ]]; then
      exec python3 "$script_dir/tools/nuvo_install.py" "${arguments[@]}"
    fi
  fi
  [[ "$repository" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$ ]] || {
    echo 'Pass --repository OWNER/REPO.' >&2; exit 2;
  }
  [[ "$release" =~ ^[A-Za-z0-9][A-Za-z0-9_.+-]*$ && "$release" != latest && "$release" != main && "$release" != master ]] || {
    echo 'Pass --release with the exact published tag when running from source.' >&2; exit 2;
  }
  for program in python3 curl openssl; do
    command -v "$program" >/dev/null || { echo "Required tool missing: $program" >&2; exit 2; }
  done
  temporary="$(mktemp -d)"
  trap 'rm -rf -- "$temporary"' EXIT
  if [[ -z "$public_key" ]]; then
    public_key="$temporary/release.pub"
    printf '%s\n' "$RELEASE_PUBLIC_KEY_PEM" > "$public_key"
  fi
  public_key="$(realpath -- "$public_key")"
  payload="$temporary/release"
  mkdir -- "$payload"
  if [[ -n "$bundle" ]]; then
    bundle="$(realpath -- "$bundle")"
    # Execute a private verified copy, even for a user-writable offline bundle.
    python3 - "$bundle" "$payload" <<'PY'
import pathlib, sys
source, target = map(pathlib.Path, sys.argv[1:])
for name, limit in (('manifest.json', 1048576), ('manifest.sig', 64), ('nuvo-install.pyz', 4194304)):
    path = source / name
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= limit:
        raise SystemExit('Invalid offline installer file: ' + name)
    with path.open('rb') as file:
        data = file.read(limit + 1)
    if len(data) > limit:
        raise SystemExit('Offline installer size limit exceeded')
    (target / name).write_bytes(data)
PY
    package_source="$bundle"
  else
    base="https://github.com/$repository/releases/download/$release"
    archive="nuvo-depth-pi5-arm64.tar.gz"
    echo "Downloading Nuvo Depth $release for Raspberry Pi 5..."
    curl --proto '=https' --proto-redir '=https' --fail --location --silent --show-error \
      --connect-timeout 20 --max-time 300 --max-filesize 268435456 \
      "$base/$archive" -o "$temporary/$archive"
    curl --proto '=https' --proto-redir '=https' --fail --location --silent --show-error \
      --connect-timeout 20 --max-time 120 --max-filesize 64 \
      "$base/$archive.sig" -o "$temporary/$archive.sig"
    openssl pkeyutl -verify -pubin -inkey "$public_key" -rawin \
      -in "$temporary/$archive" -sigfile "$temporary/$archive.sig" >/dev/null 2>&1 || {
      echo 'Release bundle signature verification failed; nothing extracted or executed.' >&2; exit 1;
    }
    python3 - "$temporary/$archive" "$payload" <<'PY'
import pathlib, re, shutil, sys, tarfile
target = pathlib.Path(sys.argv[2])
names, total = set(), 0
with tarfile.open(sys.argv[1], 'r:gz') as bundle:
    for member in bundle:
        total += member.size
        if (not member.isfile() or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.+~-]{0,190}', member.name)
                or member.name in names or len(names) >= 66 or member.size > 1024**3 or total > 4 * 1024**3 + 1024**2):
            raise SystemExit('Unsafe release bundle entry')
        names.add(member.name)
        with bundle.extractfile(member) as source, (target/member.name).open('xb') as output:
            shutil.copyfileobj(source, output)
PY
    package_source="$payload"
  fi
  openssl pkeyutl -verify -pubin -inkey "$public_key" -rawin \
    -in "$payload/manifest.json" -sigfile "$payload/manifest.sig" >/dev/null 2>&1 || {
    echo 'Release manifest signature verification failed; nothing executed.' >&2; exit 1;
  }
  python3 - "$payload" "$repository" "$release" <<'PY'
import hashlib, json, pathlib, sys
p = pathlib.Path(sys.argv[1]); manifest = json.loads((p/'manifest.json').read_text())
if manifest.get('schema_version') != 1 or manifest.get('repository') != sys.argv[2] or manifest.get('tag') != sys.argv[3]:
    raise SystemExit('Signed manifest does not match the requested release')
files = [f for f in manifest.get('files', []) if f.get('name') == 'nuvo-install.pyz']
if len(files) != 1 or not isinstance(files[0].get('size'), int) or not 0 < files[0]['size'] <= 4194304:
    raise SystemExit('Signed release is missing its bounded installer')
data = (p/'nuvo-install.pyz').read_bytes()
if len(data) != files[0]['size'] or hashlib.sha256(data).hexdigest() != files[0]['sha256']:
    raise SystemExit('Installer checksum mismatch; nothing executed')
PY
  python3 "$payload/nuvo-install.pyz" "${arguments[@]}" \
    --repository "$repository" --release "$release" --public-key "$public_key" --bundle "$package_source"
  # Never resume parsing Bash commands from the reattached terminal.
  exit 0
}
main "$@"
