# Nuvo Depth production implementation status

## Implemented software path

The repository now contains a native C++20 capture-to-products daemon path for
the Raspberry Pi 5, with a platform-neutral `CaptureSource` boundary reserved
for Luckfox Aura. The Pi backend owns both libcamera devices and the hardware
trigger, retains DMA-BUF/request leases through processing, prepares strict
content-addressed stereo calibration, rectifies both images, computes native
SGBM disparity/confidence/validity and optical-axis depth, and publishes one
associated frame identity.

Delivery includes bounded IPC v2 storage, a restricted local Unix socket with
sealed read-only frame descriptors, NuvoStream framing, HTTPS snapshot and
WebSocket streaming over Ethernet or USB Ethernet, a C ABI, a Python SDK,
whole-frame recording/replay, mDNS discovery, and a synchronized web viewer.
The native gateway has a bounded client count, TLS 1.2 minimum, strict request
limits, same-origin checks, CSP, claim-token exchange, and Secure/HttpOnly
SameSite cookies. It has no camera-device access.

Calibration tooling solves chessboard observations, validates RMS and
rectified epipolar error, binds a bundle to unit/camera/preset identities,
hashes every asset, activates through a serialized fsync-backed transaction,
and can roll back. User and factory authority are separate; factory activation
cryptographically verifies an Ed25519 signature against a separately
provisioned public-key trust store. Candidate balanced, quality, and fast
presets are present, but only measured gates can change their qualification
labels.

Native Debian debhelper metadata splits the appliance, platform, gateway, C
SDK, Python SDK, calibration, tools, and factory-policy packages. Services are
not enabled by package installation. The Pi preparation command is opt-in and
backs up an owned boot-config block. ECM gadget setup additionally requires a
real UDC, provisioned identity, explicit VID/PID, electrical approval, and no
competing gadget manager. Signed `reprepro` APT staging, SPDX generation, and a
deterministic source bundle are included.

Browser transport changes use a root-owned, socket-activated allowlist rather
than granting the gateway general privilege. Changes are staged for 60 seconds,
require confirmation, and are automatically reconciled by a systemd watchdog;
the web console cannot select the all-transports-off state.

## Hardware qualification status

The September 9, 2026 Pi 5 hardware-in-loop run passed the installed package's
configuration, 3 x 1,000-pair capture/restart, runtime-directory, and 1,000-frame
native pipeline gates. It also verified a transient systemd start, authenticated
HTTPS/SDK frame retrieval, transactional transport preference control, zero-gap
tmpfs publication, native EGL/GLES DMA-BUF rectification, and migration from the
legacy Hailo-era package. See `docs/pi5-hil-qualification-20260909.md`.

These results do not prove final-camera electrical safety, scope timing,
per-unit metric calibration, wired Ethernet or USB throughput, USB power and
host compatibility, 72-hour soak behavior, the complete package lifecycle, or
power-cut recovery. The package seed remains
`provisional_recalibration_required`; beta service use of it is an explicit,
visible engineering override.

The original Python service is retained as a rollback comparator, while the
shipping candidate is `nuvo-depthd-native.service`. Do not enable the native
target on a customer image until the hardware qualification report authorizes
release. Public publication is additionally blocked on a real maintainer
identity, top-level project license, third-party license/source review, APT
signing identity, authorized USB VID/PID, and a frozen Raspberry Pi OS
kernel/libcamera/IPA/Mesa cohort.

The following software surfaces are intentionally beta skeletons rather than
finished customer promises:

- The native Pi capture backend applies the configured fixed exposure and gain
  at startup. The shared closed-loop auto-exposure implementation remains in
  the diagnostic Python pipeline until its per-pair control epoch can be
  qualified and moved into the native capture contract without creating mixed
  stereo settings.
- The gateway's transport preference is a transactional data policy, including
  an all-output disable state available only with physical command-line
  confirmation. It does not yet bind separate listeners or configure a host
  network manager; actual Ethernet/USB interface enforcement and payload
  admission require measurements on the shipping network cohort.
- The web console is a functional claimed viewer, health display, diagnostic
  view, and transport control. Preset selection, exposure tuning, and guided
  calibration pages are workflow skeletons; the validated command-line
  calibration transaction is the current authority.
- Recording uses the bounded, versioned `NVR1` frame-set container. An MCAP
  exporter remains an extension point and is not advertised as implemented.
- The admission controller and bounded IPC/wire primitives are tested as
  libraries, while the first native gateway streams the latest complete
  snapshot. Product negotiation and measured per-interface admission still
  need to be connected after the Pi throughput campaign.

## Next hardware work

Add the nine required manual artifacts listed in
`docs/pi5-hil-qualification-20260909.md` to the retained qualification evidence
directory. Regenerate its report with `--resume --skip-automated`; stored
automated gate identities and hashes are verified before reuse. The generated
report remains the single release authority.
