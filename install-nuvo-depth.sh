#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Meridian Robotics. See LICENSE for terms and warranty notice.
# Run a reviewed checkout, or download the signed installer from a pinned release.
set -euo pipefail
umask 077

# Public Nuvo source and signed release assets; override with --repository.
DEFAULT_REPOSITORY="arputhamips/nuvo-depth"
# SHA-256 of DER public key: 6cd440dd00741e095a4dc436001463506eab7e295dcf608a156311e46344ee15
# Private release key is held separately from the repository's SSH deploy key.
RELEASE_PUBLIC_KEY_PEM='-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAQ0zwQ1yw+Tu532eeDs+iBZpSfVBCobIrCbJzlHaGRWk=
-----END PUBLIC KEY-----'

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$script_dir/tools/nuvo_install.py" ]]; then
  exec python3 "$script_dir/tools/nuvo_install.py" "$@"
fi

repository="$DEFAULT_REPOSITORY"
release=""
public_key=""
arguments=("$@")
while (($#)); do
  case "$1" in
    --repository|--release|--public-key)
      (($# >= 2)) || { echo "Missing value for $1" >&2; exit 2; }
      case "$1" in
        --repository) repository="$2" ;;
        --release) release="$2" ;;
        --public-key) public_key="$2" ;;
      esac
      shift 2 ;;
    --help|-h)
      cat <<'HELP'
Nuvo Depth terminal installer
  --repository OWNER/REPO  Override arputhamips/nuvo-depth
  --release TAG           Exact published release; never main/latest
  --public-key PATH       Independently trusted Ed25519 release public key
  --factory               Manufacturing preset; no selection prompts
  --wifi-country CC       Actual operating country (factory input if OS unset)
  --reboot                Permit a required reboot
  --dry-run               Inspect without making changes
  --bundle DIRECTORY      Use an extracted signed release offline (checkout only)
Download the release installer and trust key as described in docs/install.md.
HELP
      exit 0 ;;
    *) shift ;;
  esac
done
[[ "$repository" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$ ]] || {
  echo 'Configure DEFAULT_REPOSITORY or pass --repository OWNER/REPO.' >&2; exit 2;
}
[[ "$release" =~ ^[A-Za-z0-9][A-Za-z0-9_.+-]*$ && "$release" != latest && "$release" != main && "$release" != master ]] || {
  echo 'Pass --release with the exact published tag.' >&2; exit 2;
}
for program in python3 curl openssl; do
  command -v "$program" >/dev/null || { echo "Required tool missing: $program" >&2; exit 2; }
done
temporary="$(mktemp -d)"
trap 'rm -rf -- "$temporary"' EXIT
if [[ -z "$public_key" ]]; then
  [[ -n "$RELEASE_PUBLIC_KEY_PEM" ]] || {
    echo 'Release trust is not configured. Pass an independently trusted --public-key.' >&2; exit 2;
  }
  public_key="$temporary/release.pub"
  printf '%s\n' "$RELEASE_PUBLIC_KEY_PEM" > "$public_key"
fi
public_key="$(realpath -- "$public_key")"
# Release assets use the exact, explicitly selected GitHub release tag.
base="https://github.com/$repository/releases/download/$release"
for asset in manifest.json manifest.sig; do
  limit=1048576
  [[ "$asset" != manifest.sig ]] || limit=64
  curl --proto '=https' --proto-redir '=https' --fail --location --silent --show-error \
    --connect-timeout 20 --max-time 120 --max-filesize "$limit" "$base/$asset" -o "$temporary/$asset"
done
openssl pkeyutl -verify -pubin -inkey "$public_key" -rawin \
  -in "$temporary/manifest.json" -sigfile "$temporary/manifest.sig" >/dev/null 2>&1 || {
  echo 'Release manifest signature verification failed; nothing executed.' >&2; exit 1;
}
python3 - "$temporary/manifest.json" "$repository" "$release" <<'PY'
import json, sys
p=json.load(open(sys.argv[1]))
if p.get('schema_version')!=1 or p.get('repository')!=sys.argv[2] or p.get('tag')!=sys.argv[3]:
    raise SystemExit('Signed manifest does not match the requested release')
files=[f for f in p.get('files',[]) if f.get('name')=='nuvo-install.pyz']
if len(files)!=1 or not isinstance(files[0].get('size'),int) or not 0<files[0]['size']<=4*1024*1024:
    raise SystemExit('Signed release is missing its bounded installer')
PY
curl --proto '=https' --proto-redir '=https' --fail --location --silent --show-error \
  --connect-timeout 20 --max-time 120 --max-filesize 4194304 \
  "$base/nuvo-install.pyz" -o "$temporary/nuvo-install.pyz"
python3 - "$temporary" <<'PY'
import hashlib,json,pathlib,sys
p=pathlib.Path(sys.argv[1]); m=json.loads((p/'manifest.json').read_text())
f=next(f for f in m['files'] if f['name']=='nuvo-install.pyz'); data=(p/f['name']).read_bytes()
if len(data)!=f['size'] or hashlib.sha256(data).hexdigest()!=f['sha256']:
    raise SystemExit('Installer checksum mismatch; nothing executed')
PY
python3 "$temporary/nuvo-install.pyz" "${arguments[@]}" \
  --repository "$repository" --release "$release" --public-key "$public_key"
