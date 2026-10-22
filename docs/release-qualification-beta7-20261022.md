# Beta 7 draft qualification — 2026-10-22

**Beta 7 remains an unpublished draft prerelease.** Both exact signed package
profiles passed physical Pi 5 first-use, calibration import/recovery and reinstall
checks. The host's original installation, identity, calibration and network
configuration were preserved and restored. The approved public installer still
selects beta 6 through `deployment/public-release.json`.

- Source/tag commit: [`6957a472965893723f882f821c03e3b8f45ec0d3`](https://github.com/arputhamips/nuvo-depth/commit/6957a472965893723f882f821c03e3b8f45ec0d3).
- Candidate: [`v0.1.1-beta.7` draft](https://github.com/arputhamips/nuvo-depth/releases/tag/untagged-8fb78f1bd4cca478bb7f).
- [ARM64 builds, clean installations and signing](https://github.com/arputhamips/nuvo-depth/actions/runs/39860685754).
- [Machine-readable package, hardware and restoration evidence](release-qualification-beta7-20261022.json).
- [Readiness and supported import contract](calibration-readiness.md).

## Exact candidate and automated checks

| Profile | Runtime packages | Signed bundle size | ARM64 CTest |
| --- | ---: | ---: | ---: |
| Full appliance | 8 | 2,501,575 bytes | 61/61 |
| Minimal | 1 | 1,720,192 bytes | 59/59 |

All six uploaded draft assets were downloaded and matched their GitHub SHA-256
digests. Both detached Ed25519 signatures, signed manifests, all nine Debian
package hashes/versions, exact installer pins and source records verified against
the committed public key. Both source archives matched byte-for-byte. Every
runtime package is `0.1.1~beta.7`; both build records identify the tag commit.

The full bundle SHA-256 is
`e1491bf3bd4590a57e181d8811982f81e290e014043cb16aa32a9faf76c042b4`;
the minimal bundle SHA-256 is
`07638357223f712a009183862c3c985f3da99da99c30bdc76f09875ce2842b17`.

Host CTest passed **61/61** checks. Ten browser fixture checks exercised first
depth, saved metadata, invalid/stale/disconnected output, pending calibration,
accepted calibration with unavailable capture, specific readiness errors and
collapsed advanced controls. These browser checks used deterministic API
fixtures; the physical HTTPS/SDK checks below used real camera output.

The wiki built **31 HTML pages / 162 files**, passed release/setup consistency
and link/asset checks, and passed automated accessibility/layout checks across
**12 page/viewport combinations**, including the new readiness guide on desktop
and mobile. The site was deployed from the website repository's `main` branch.

The clean ARM64 runtime checks verified package installation and the minimal
dependency boundary: no calibration solver, NumPy, Python OpenCV, Nuvo web
console, network SDK or encoder package. Shared import/status control uses the
standard library and the existing Ed25519 verification dependency. Native
OpenCV/libcamera remain part of the depth runtime.

## Physical first-use and calibration checks

Tests used the physical dual-IMX296 head on `nuvomow.local`, kernel
`6.12.75+rpt-rpi-2712`, with two disposable copy-on-write Pi OS Trixie filesystems.
They had separate package databases, configuration, identity and systemd instances,
using the host's OS libraries. These hardware environments were not clean
dependency-isolation environments; that evidence comes from ARM64 CI above.
Camera devices and trigger interfaces were temporarily handed over with a host
restore timer armed. Gateway tests used a private network namespace and its
engineering loopback option. Host SSH/network settings were retained.

| Check | Result |
| --- | --- |
| Signed minimal installer dry-run and installation | Passed; fresh capture deliberately disabled with an explained next action |
| Signed full interactive installation | Passed; choices explained before selection, automatic HTTPS connection, shared pending readiness |
| Explicit preview / capture disable | Passed in both profiles; provisional frame authority stayed `0` |
| Wrong-device dry-run and corrupt bundle import | Rejected before selection changed |
| Compatible original bundle import | Passed in both profiles; new daemon run and two advancing associated depth/validity frames verified before success |
| Accepted output | 728×544, authority `1`, original bundle hash and advancing identities/timestamps |
| Minimum/maximum valid pixels in six accepted samples | Minimal: 226,225–234,375; full: 232,965–238,993 |
| One-shot service restart failure | Import failed and restored the previous selection, policy and working service |
| Activator terminated after durable selection | Startup restored the previous disabled policy and refused unintended capture |
| Rollback to pre-import disabled capture | Passed in both profiles |
| Overview/status interfaces | Physical HTTPS setup readiness, CLI and daemon bundle metadata agreed |
| Python first-use helper | Installed, saved useful fresh depth to NVR1, and replayed offline with original calibration metadata |
| Console rollback through sandboxed system agent | Passed, followed by successful verified reimport |
| Signed same-version reinstall | Both profiles preserved all eight checked configuration, identity and bundle files byte-for-byte |
| Full interactive reinstall completion | Reported accepted calibration after preserving the existing selection |

Each profile's recorded raw checks include six preview frames, six accepted
frames and ten frames after failure/interruption recovery. Additional activation,
SDK and installer verification frames were also consumed. Capture/processing
error counters remained zero. An idle status can show zero current valid pixels
after subscribers disconnect; the saved sample metadata and validity products
establish useful output for the checks above.

## Calibration authority and host restoration

The shared default remains provisional and reuses the unchanged beta 5 geometry
from bundle `8adf4c8103f1fb7f2811a7d483669cc99c8c7132902700ebb3fd464d19158cf2`.
Its seed/provenance under `config/calibration/nuvo_depth_100mm/` was unchanged by
the readiness/import work. Published beta 6 contained an earlier shared seed;
its historical qualification records were retained unchanged.

The host's preserved legacy YAML pair continues to run with its original loose
calibration hash. Its adjacent original bundle names a different unit UUID from
the current host installation. Import correctly rejected that binding. Accepted
path tests used the genuine original bundle and its original UUID only inside
the isolated test systems, on the unchanged physical head. No manifest, geometry,
validation record or host identity was rewritten to make acceptance pass. The
host's identity history needs reconciliation before that old bundle can be
accepted against its current UUID; a software update alone does not require
recalibration.

All **16 protected host file hashes matched** after testing. The original minimal
`0.1.1~beta.6` installation and validated YAML files were restored, with **10 new
capture pairs in one second**, zero capture/processing errors, a clean dpkg audit
and `throttled=0x0`. Disposable roots, temporary test helpers and restore timers
were removed; the temporary overlay module was unloaded. Private backups and
diagnostic logs remain on the Pi.

These results do not qualify a fresh Pi OS image, physical kernel reboot/power
cycle, external Wi-Fi handoff, absolute metric accuracy, electrical trigger timing,
long-term endurance or a 1 GiB Pi. Accepted calibration means compatible declared
validation; independent known-distance accuracy and production qualification
remain separate evidence. Beta 7 was not published or promoted to the public
installer reference.
