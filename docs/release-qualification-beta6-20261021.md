# Beta 6 draft qualification — 2026-10-21

**v0.1.1-beta.6 is a draft prerelease.** The full and minimal signed bundles
passed signature, provenance, package and physical-camera checks. It has not
been published or marked latest. Beta 5 remains the public preview.

- Source: [`5db24de8bee229fb2d3940a8f1e503e96e494b36`](https://github.com/arputhamips/nuvo-depth/commit/5db24de8bee229fb2d3940a8f1e503e96e494b36).
- Debian version: `0.1.1~beta.6` for all nine packages across the two exclusive profiles.
- [Draft release](https://github.com/arputhamips/nuvo-depth/releases/tag/untagged-7308751acdb65e55adc1).
- [Actions build, clean-install and signing evidence](https://github.com/arputhamips/nuvo-depth/actions/runs/39676729482).
- [Machine-readable verification and hardware results](release-qualification-beta6-20261021.json).

## Packages and installers

| Profile | Signed bundle | Runtime packages | Separate installer |
| --- | --- | --- | --- |
| Full appliance | `nuvo-depth-pi5-arm64.tar.gz` (2,156,239 bytes) | 8 | `install-nuvo-depth.sh` |
| Minimal raw depth | `nuvo-depth-minimal-pi5-arm64.tar.gz` (1,402,469 bytes) | 1 | `install-nuvo-depth-minimal.sh` |

Each bundle has its own detached Ed25519 signature. All six uploaded assets
were downloaded from the draft and verified against the committed public key.
Both manifests, every file hash, exact Debian package metadata, installer pins
and build records passed verification. Both bundles retain identical matching
source archives. Full Actions package bytes exactly match the signed full cohort.

The minimal `.deb` is **344,140 bytes**, SHA-256
`5f97fa4eb5b74cfa808224d351df268bee7bf30a96cb94afac75a5c6bdeb6aca`. It installs the existing native pipeline and terminal
interface. The full appliance runtime and installer behavior remain unchanged.

## Automated checks

- Full ARM64 build: **56/56 CTest checks**, plus disposable runtime installation.
- Minimal ARM64 build: **54/54 CTest checks**, package payload/dependency inspection
  and clean runtime installation with all required service users/groups resolved.
- Clean minimal runtime excludes gateway, appliance tools, SDK, calibration tools,
  NetworkManager, hotspot/mDNS services, Python OpenCV/NumPy and the `ffmpeg` package.
- **87 local installer/release/CLI tests** passed, including both signed download
  profiles in one release and rejection of mixed package cohorts.
- Modified workflow YAML, shell blocks and actionlint passed. Builds and signing
  remain manual; documentation edits do not dispatch them.
- The initial minimal build hit an existing PTY test race after its interactive
  fixture completed. The same tagged source passed all tests on retry; signing
  remained blocked until both profiles and runtime installation checks passed.

## Physical Pi checks

Both profiles used the Pi 5's physical dual IMX296 cameras at 728×544 on kernel
`6.12.75+rpt-rpi-2712`, with separate disposable ARM64 Trixie filesystems.

| Check | Result |
| --- | --- |
| Full exact eight-package cohort installation | Passed, clean dpkg audit |
| Full normal signed installer, interactive dry-run | Passed |
| Full raw depth + validity + mono stream | 60 unique frames, 9.99953 Hz by capture timestamps |
| Full valid depth pixels | 158,078–176,844 per frame |
| Full browser session and served interface | Passed |
| Full optional H.264 output | Passed after selecting `both` with the existing CLI |
| Minimal signed installer dry-run and cold installation | Passed; installer verified fresh depth |
| Minimal raw depth + validity stream | 60 unique frames, 9.99956 Hz by capture timestamps |
| Minimal additional sustained output | 300 frames in 30.184 seconds |
| Minimal live exposure/matcher controls | Passed |
| Minimal service restart, stop/trigger disable and idle demand | Passed |
| Minimal same-version signed reinstall | Config/UUID preserved; dpkg verify/audit clean |
| Minimal enabled startup after isolated userspace restart | Passed; new run UUID and fresh depth |

Trigger, request requeue and scheduler processing error counters were zero.
The full gateway used its engineering loopback option inside a private network
namespace. The test fixture supplied Pi OS device groups and physical device
access to the bare Debian base. Its package configuration required a `/proc`
mount; writable PWM access was retained separately from network isolation.

## Host restoration and limits

All **25 protected host file hashes** and all original package versions
were unchanged. Configuration, identity, calibration, boot files, network profiles
and the existing executables were preserved. Native and gateway services were
restored; capture advanced by **10 pairs in one second**, with zero errors,
a clean dpkg audit and `throttled=0x0`. Both disposable roots and test-only
host helpers were removed, and restore timers were stopped. Private backups and
diagnostics remain on the Pi.

This qualifies the exact draft packages and camera output in the described
containment. It does not qualify a fresh Pi OS image, physical kernel reboot,
power cycle, external network provisioning, metric calibration accuracy,
electrical trigger timing or a 1 GiB Pi. The test Pi has 16 GiB RAM. No full
interactive host upgrade was applied; the minimal signed install/reinstall and
isolated startup were exercised directly.
