# Minimal raw-depth package qualification — 2026-10-21

The minimal package installed through its separate installer and produced raw
depth on the physical Pi 5 camera head. The full appliance was restored after
testing: all 24 saved configuration, identity, calibration and executable hashes
matched, its Nuvo package versions matched, both capture and gateway were active,
and capture advanced by ten pairs in the final one-second health check.

This is Actions-artifact acceptance. The package retains the existing
`0.1.1~beta.5` build version; its source and SHA-256 distinguish it from published
beta 5. No tag, GitHub release or signing job was created.

## Build and scope

- Source: `061a43325f12e10e27d8a138fad5f63a41e1ab22`.
- [Minimal ARM64 build and clean runtime install](https://github.com/arputhamips/nuvo-depth/actions/runs/39672182788):
  54/54 CTest entries passed; fresh Trixie installation, CLI/config validation,
  service user/group resolution and dependency exclusions passed.
  Artifact: `nuvo-minimal-arm64`.
- [Full appliance regression build](https://github.com/arputhamips/nuvo-depth/actions/runs/39670044057),
  source `c2356d9`: 56/56 tests passed and all eight runtime packages installed
  in the existing container smoke test. Subsequent changes affect the minimal
  profile, its tests and evidence.
- Package: `nuvo-depth-minimal_0.1.1~beta.5_arm64.deb`, 344,280 bytes;
  installed payload approximately 1.2 MiB, excluding OS dependencies.
- SHA-256:
  `774772af439e45a92b58f7da6a4a2255e807327c4a3171c4734a9f2767765747`.

The minimal build uses the existing CPU rectifier, matcher, capture backend,
depth conversion and IPC. No source under `apps/`, `src/`, `platform/` or
`include/` was modified. The full installer and administrative CLI were preserved.
The new reader links the existing local frame client.

The package excludes the web UI/gateway, network SDKs, Wi-Fi onboarding, system
agent, transport rollback, USB networking, calibration workflow, encoder and GPU
rectifier. Native OpenCV, libcamera and their distribution dependencies remain.
Upstream libraries can require additional image-format support libraries.

## Physical Pi acceptance

Tests used a separate ARM64 Trixie filesystem with its own package database,
configuration, identity and systemd instance on the existing Pi 5, kernel
`6.12.75+rpt-rpi-2712`. Real camera devices and trigger interfaces were exposed
to that instance. Camera ownership was temporarily handed over with a host
restore timer armed. Boot firmware was read-only; host SSH/network configuration
was retained. An initial startup failure exposed an inherited `render` group
requirement; the rebuilt CPU-only service removes it. Accepted runs used the
rebuilt package without service edits.

| Check | Result |
| --- | --- |
| Installer | Artifact checks, Pi/kernel preflight, APT install, preparation, service startup and three fresh verification frames passed |
| Raw capture | 728 × 544 float32 Z depth in metres, validity and metadata; initial capture had 165,381 valid pixels |
| Stream | 60 unique, increasing frame identities/timestamps; 9.99955 Hz by capture timestamps; final frame age about 150 ms |
| Longer stream | 300 further fresh frames in 30.14055 seconds |
| Configuration | Live 6,000 µs exposure and expected matcher revision applied; original controls restored byte-for-byte |
| Restarts | Three restart/capture cycles with distinct run UUIDs |
| Stop/start | PWM disabled on stop; fresh depth returned after start |
| Idle | Capture continued while depth/matcher work stopped without readers |
| Startup | Enabled service started automatically on a new isolated OS boot and produced depth |
| Reinstall | Same-version reinstall preserved configuration and UUID; dpkg verification/audit clean |
| Full-install guard | Host dry run rejected the installed full cohort before changes |
| Errors | Zero trigger, request-requeue and scheduler-processing failures |
| Thermal | 51.8 °C after functional checks; host `throttled=0x0` after restoration |
| Memory | 135,936 KiB process RSS/high-water sample after startup/capture; cgroup metrics unavailable in containment |

Defaults were SGBM `3way`, search range 128, block size 5, two OpenCV threads,
no median filter, 728 × 544, capture target 10 Hz. These differ from earlier
full-appliance `hh` benchmarks; no performance gain from package removal is
established. The reusable check is
[`tests/hil/pi5_minimal_check.py`](../tests/hil/pi5_minimal_check.py); counters,
metadata, settings and preservation results are in the
[JSON evidence](minimal-package-qualification-20261021.json).

The host kernel was not rebooted and no SD image was reflashed. The isolated OS
was restarted to test enabled-service startup; its supervisor required an
explicit restart after a guest reboot request. Physical power-cut recovery and
fresh Pi OS image boot remain separate checks. The provisional rectification
seed does not establish per-unit metric accuracy or electrical trigger timing.
The 16 GB test Pi does not qualify a 1 GB unit.

The disposable root, transfer files and three temporary host test packages were
removed, with zero restore timers left. Private backups/logs remain on the Pi.
No credentials or private host configuration were added to the repository.
The existing full appliance remains running.
