#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run only inside the disposable Debian Trixie ARM64 CI container.
set -euo pipefail
test "$(dpkg --print-architecture)" = arm64
. /etc/os-release
test "$VERSION_CODENAME" = trixie
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gnupg git build-essential debhelper nodejs
# The old raspberrypi.gpg.key has SHA-1 self-signatures rejected by Trixie sqv.
# Extract the current archive keyring from a versioned, checksum-pinned package.
key=/tmp/raspberrypi-archive-keyring.deb
curl --proto '=https' --proto-redir '=https' --fail --location --retry 3 \
  https://archive.raspberrypi.com/debian/pool/main/r/raspberrypi-archive-keyring/raspberrypi-archive-keyring_2025.1+rpt1_all.deb -o "$key"
echo "2e727149d7acb8cc7f604e66d0049161039c8aa1eaf1175e54f9e69d963d60e4  $key" | sha256sum --check --strict
dpkg-deb --extract "$key" /tmp/nuvo-rpi-keyring
install -m 0644 /tmp/nuvo-rpi-keyring/usr/share/keyrings/raspberrypi-archive-keyring.pgp /usr/share/keyrings/raspberrypi-archive-keyring.pgp
cat > /etc/apt/sources.list.d/raspi.sources <<'SOURCES'
Types: deb
URIs: https://archive.raspberrypi.com/debian/
Suites: trixie
Components: main
Signed-By: /usr/share/keyrings/raspberrypi-archive-keyring.pgp
SOURCES
apt-get update
apt-get build-dep -y --no-install-recommends .
dpkg-checkbuilddeps
