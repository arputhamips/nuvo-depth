# Depth Daemon

Public source and release repository: [arputhamips/nuvo-depth](https://github.com/arputhamips/nuvo-depth).

Native stereo-depth runtime for the Nuvo Raspberry Pi 5 and dual
hardware-triggered IMX296 cameras. Stereo processing is selected through a
ROS-free backend interface; the production backend is OpenCV CPU SGBM. Capture,
pair validation, rectification, matching, and buffer management happen outside
DDS. A small boundary adapter publishes only finished products.

The product build is depth-only. Semantic perception and accelerator experiments
are not built, installed, provisioned, or required by the Debian package.

## License

Nuvo-authored software, including the SDKs and installer, is licensed under
**GNU GPL version 3 or any later version** (`GPL-3.0-or-later`). Copyright
2026 Meridian Robotics. See [LICENSE](LICENSE) for the full terms and
[NOTICE](NOTICE) for scope and third-party exceptions. The software is provided
without warranty as described in the license. Third-party software and assets
retain their existing licenses and copyright notices.

When publishing binary releases, provide matching Corresponding Source and
build/install instructions alongside the packages. Use immutable source tags
for the exact released version. Device owners must have a documented way to
build and install modified software; release-signature verification must not
require them to possess the vendor's private signing key. Local builds may use
their own release key and the installer's `--public-key` / `--bundle` options.

## Appliance status

The repository now contains the complete software skeleton for a Pi 5 beta:
native dual-libcamera capture and GPIO-trigger ownership, strict stereo pairing,
calibration-aware rectification and CPU SGBM, associated left/right/disparity/
depth/confidence/validity products, bounded local IPC, a TLS gateway and web
console, C and Python SDKs, calibration/recording tools, systemd units, and
split Debian packaging. `CaptureSource` is the platform boundary for a future
Luckfox Aura adapter; product semantics above that boundary remain shared.

This is not yet a factory release. The September 9, 2026 Pi 5 rig run passed all
four automated installed-package gates, including 3 x 1,000 hardware-triggered
pairs and 1,000 zero-gap native depth frames. The checked-in calibration remains
provisional, the higher-rate presets are unqualified, and trigger electrical
evidence, powered USB-C, wired throughput, metric accuracy, thermal endurance,
full package lifecycle, and recovery tests remain pending. See
[`docs/pi5-hil-qualification-20260909.md`](docs/pi5-hil-qualification-20260909.md).
The qualification report is the release authority; source code or a successful
automated run cannot waive a manual gate.

Useful entry points:

For setup and normal operation, start with the
[documentation index](docs/README.md) and [Pi 5 user guide](docs/user-guide.md).
The [terminal/factory installer guide](docs/install.md) covers the shared setup
script and direct installation from signed GitHub releases. The
[release operations guide](docs/releases.md) covers ARM64 package builds,
Ed25519 signing, source archives and publication.
The current live-device evidence, performance, and release blockers are in the
[September 14 release-readiness report](docs/pi5-release-readiness-20260914.md).
Calibration profile production is a separate advanced process documented in
[the calibration guide](docs/advanced-calibration.md). The official Debian and
signed vendor-repository paths are in the
[APT publication roadmap](docs/debian-archive-roadmap.md).

Build presets, dependencies, and the consolidated history of the old
`build-step*` folders are in the [build and test guide](docs/build-and-test.md).

```bash
cmake --preset development-full
cmake --build --preset development-full -j
ctest --preset development-full

depthd --check-config config/nuvo_pi5_depth_100mm.yaml
depthd --pipeline-check config/nuvo_pi5_depth_100mm.yaml \
  --left LEFT.png --right RIGHT.png --output-dir replay-output \
  --allow-provisional-depth

sudo nuvoctl identity provision
sudo nuvoctl platform preflight
sudo nuvoctl platform prepare --platform pi5 --apply
```

The native appliance target is deliberately not enabled during package
installation. Enable it only on a provisioned engineering unit, then remove the
provisional-depth override from the shipping service after a validated per-unit
calibration is active.

For a fresh review session, use
[`docs/nuvo-depth-production-oracle-prompt.md`](docs/nuvo-depth-production-oracle-prompt.md).
For the assembled Pi, use
[`docs/hardware-qualification-handoff-prompt.md`](docs/hardware-qualification-handoff-prompt.md).

## Current Pi 5 candidate profile

- Calibration profile identity: `balanced_728x544_10`, retained for compatibility.
- Capture target: **728 x 544 / 10 Hz**, with YUV420 ISP luma and a 180-degree
  orientation on the shared GPIO 17 trigger. The target is not a depth-rate guarantee.
- Default matcher: full-grid HH, 96 disparities, median 5. Historical Pi evidence
  measured 342.01 ms median compute and 2.84 depth frames/s at a 60 Hz capture target.
  Current profile throughput remains unqualified; see the
  [performance profile catalog](docs/performance-profiles.md).
- Rectified focal length: 493.542046188 px.
- Nominal product baseline: **0.100 m**.
- Appliance frame sets contain associated rectified left and right images,
  disparity, metric optical-axis depth, confidence, and validity.
- The legacy ROS/native-output adapter remains available for compatibility, but
  it is not the appliance API or the configuration authority.

See [architecture options](docs/architecture-options.md) and the historical
[Nuvo compatibility contract](docs/nuvo-compatibility.md). The first measured runtime is
documented in the [CPU SGBM baseline](docs/cpu-sgbm-baseline.md), with the full
live camera path in the [CPU pipeline profile](docs/cpu-live-pipeline-profile.md)
and the corrected [async zero-copy trigger results](docs/async-zero-copy-trigger-results.md).
The settled navigation matcher settings and the complete geometry/filter sweep
are recorded in the [CPU SGBM production profile](docs/cpu-sgbm-production-profile.md).
The downstream ROS-free projection and voxel/cluster filtering contract is in
the [native compact obstacle cloud](docs/compact-obstacle-cloud.md). The first
camera-to-mmap run is recorded in the
[native output live validation](docs/native-output-live-validation.md).
Host packaging, daemon-owned calibration provisioning, systemd startup,
and atomic rollback are documented in the
[production deployment guide](deployment/README.md).
The current product geometry, operating point, and hardware acceptance gates
are recorded in the [Pi 5 100 mm depth profile](docs/pi5-depth-100mm-profile.md).
The opt-in native scheduler, IPC v2 ownership core, common Ethernet/USB-IP wire
protocol, and SDK decoder skeleton are tracked in the
[native runtime/IPC/gateway status](docs/native-runtime-ipc-gateway.md).
The optional post-depth H.264/MJPEG monitoring path, raw/encoded/both modes, web controls,
wire envelope, and measured Pi 5 profiles are documented in the
[Pi 5 image-stream encoding guide](docs/pi5-image-stream-encoding.md).

The checked-in 100 mm calibration files are explicitly provisional. They retain
the existing camera-unit intrinsics and encode the new projection baseline, but
the assembled 100 mm product must be stereo-calibrated before metric depth is
treated as production-calibrated.

Image rectification has its own backend registry, separate from stereo
matching. The production `cpu_opencv` implementation is documented in the
[rectification backend guide](docs/rectification-backends.md).

## Repository shape

```text
include/depthd/       Stable ROS-free data contracts
src/                  ROS-free backend implementations
tests/                Dependency-light contract tests
config/               Runtime profile and reference calibration
docs/                 Decisions, audit notes, and acceptance criteria
models/               Optional research model sources (not in product package)
third_party/          Optional research sources (not in product package)
```

The installed compatibility result transport is a dependency-free, versioned four-slot mmap ring under
`/run/depthd`. Its authoritative cross-process layout lives in
`include/depthd/ipc/result_ring_layout.hpp`; the ROS bridge vendors that exact
ABI and publishes only the newest complete sequence. Camera runtime remains
separate from the ROS/ament bridge.

IPC v2 is the native appliance boundary under `/run/nuvo-depth`. The original
IPC v1/Python service is retained only as a rollback comparator until hardware
parity qualification authorizes final removal.

Legacy semantic research code remains available only when explicitly building
with `-DDEPTHD_WITH_SEMANTICS=ON`. Those targets are excluded from product
installation and Debian packages.

## Build the current contract test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

When OpenCV development packages are present, this also builds
`depthd_sgbm_benchmark`. It accepts a rectified 728 x 544 pair and reports
per-stage latency plus process and normalized four-core CPU utilization.

## Non-goals

- No raw left/right ROS image loop.
- No Python in the Pi frame loop.
- No full-rate dense `PointCloud2`.
- No semantic perception or Hailo dependency in the product package.
