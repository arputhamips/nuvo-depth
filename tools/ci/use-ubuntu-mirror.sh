#!/usr/bin/env bash
# Only disposable hosted Ubuntu runners: avoid a repeatedly stalled Azure mirror.
set -euo pipefail
test "${GITHUB_ACTIONS:-}" = true
. /etc/os-release
test "$ID:$VERSION_ID" = ubuntu:24.04
python3 - <<'PY'
from pathlib import Path
for path in (Path('/etc/apt/sources.list'), Path('/etc/apt/sources.list.d/ubuntu.sources')):
    if not path.is_file():
        continue
    text=path.read_text()
    text=text.replace('http://azure.archive.ubuntu.com/ubuntu','https://archive.ubuntu.com/ubuntu')
    text=text.replace('https://azure.archive.ubuntu.com/ubuntu','https://archive.ubuntu.com/ubuntu')
    path.write_text(text)
print('Disposable CI runner uses the official Ubuntu archive; package signature policy is unchanged.')
PY
