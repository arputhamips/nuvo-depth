#!/usr/bin/env bash
# Host-only dependencies; never enables Pi capture or installs a Nuvo package.
set -euo pipefail
profile="${1:---minimal}"
case "$profile" in --minimal|--full) ;; *) echo 'Usage: bash tools/dev/bootstrap-host.sh --minimal|--full' >&2; exit 2;; esac
. /etc/os-release
case "$ID:$VERSION_ID" in debian:13|ubuntu:24.04) ;; *) echo 'Supported clean hosts: Debian 13 Trixie or Ubuntu 24.04 (also under WSL2).' >&2; exit 2;; esac
elevate=()
if [ "$(id -u)" != 0 ]; then elevate=(sudo); fi
"${elevate[@]}" apt-get update
packages=(build-essential cmake ninja-build pkg-config git ca-certificates curl xz-utils openssl python3 python3-venv python3-cryptography python3-yaml nodejs npm)
if [ "$profile" = --full ]; then
  packages+=(libyaml-cpp-dev libopencv-dev libavcodec-dev libavutil-dev libswscale-dev libboost-system-dev libssl-dev python3-numpy python3-opencv)
fi
"${elevate[@]}" apt-get install -y --no-install-recommends "${packages[@]}"
# Ubuntu 24.04's Node 18 cannot run the pinned browser tools. Use the official
# archive only when the existing runtime is too old; verify the frozen digest.
if ! node -e 'process.exit(Number(process.versions.node.split(".")[0]) < 20 ? 1 : 0)'; then
  case "$(uname -m)" in
    x86_64) architecture=x64; digest=9a6bc82f9b491279147219f6a18add1e18424dce90d41d2a5fcd69d4924ba3aa ;;
    aarch64) architecture=arm64; digest=0f3550d58d45e5d3cf7103d9e3f69937f09fe82fb5dd474c66a5d816fa58c9ee ;;
    *) echo 'Browser development requires Node 20+ on this architecture.' >&2; exit 2 ;;
  esac
  archive="$(mktemp)"; trap 'rm -f "$archive"' EXIT
  curl --fail --silent --show-error --location "https://nodejs.org/dist/v22.22.1/node-v22.22.1-linux-$architecture.tar.xz" --output "$archive"
  printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --status
  "${elevate[@]}" mkdir -p /opt/nuvo-host-node-22.22.1
  "${elevate[@]}" tar -xJf "$archive" --strip-components=1 -C /opt/nuvo-host-node-22.22.1
  for executable in node npm npx; do
    "${elevate[@]}" ln -sf "/opt/nuvo-host-node-22.22.1/bin/$executable" "/usr/local/bin/$executable"
  done
  hash -r
fi
python3 - <<'PY'
import json, pathlib, platform, subprocess
version=subprocess.check_output(['cmake','--version'],text=True).splitlines()[0].split()[-1]
if tuple(map(int,version.split('.')[:2])) < (3,25): raise SystemExit('CMake 3.25 or later is required')
directory=pathlib.Path('output/dev-environment'); directory.mkdir(parents=True,exist_ok=True)
document={'schema_version':1,'os_release':pathlib.Path('/etc/os-release').read_text(),
          'kernel':platform.release(),'machine':platform.machine(),'python':platform.python_version(),
          'cmake':version,'node':subprocess.check_output(['node','--version'],text=True).strip(),
          'version_policy':'Named stable distribution packages including security updates; Node 22.22.1 official digest-pinned fallback when distro Node is below 20; resolved versions recorded for each run.',
          'packages':subprocess.check_output(['dpkg-query','-W','-f=${binary:Package}\t${Version}\n'],text=True)}
(directory/'manifest.json').write_text(json.dumps(document,indent=2)+'\n')
PY
