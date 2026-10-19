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
key=/tmp/raspberrypi-archive-key
curl --proto '=https' --proto-redir '=https' --fail --location --retry 3 \
  https://archive.raspberrypi.com/debian/raspberrypi.gpg.key -o "$key"
echo "76603890d82a492175caf17aba68dc73acb1189c9fd58ec0c19145dfa3866d56  $key" | sha256sum --check --strict
gpg --dearmor < "$key" > /usr/share/keyrings/raspberrypi-archive-keyring.gpg
cat > /etc/apt/sources.list.d/raspi.sources <<'SOURCES'
Types: deb
URIs: https://archive.raspberrypi.com/debian/
Suites: trixie
Components: main
Signed-By: /usr/share/keyrings/raspberrypi-archive-keyring.gpg
SOURCES
apt-get update
apt-get build-dep -y --no-install-recommends .
dpkg-checkbuilddeps
