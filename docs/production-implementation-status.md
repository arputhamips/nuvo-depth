# Nuvo Depth production implementation status

## Implemented software path

The repository now contains a native C++20 capture-to-products daemon path for
the Raspberry Pi 5, with a platform-neutral `CaptureSource` boundary reserved
for Luckfox Aura. The Pi backend owns both libcamera devices and the hardware
trigger, retains DMA-BUF/request leases through processing, prepares strict
content-addressed stereo calibration, rectifies both images, computes native
SGBM disparity/confidence/validity and optical-axis depth, and publishes one
associated frame identity.

Delivery includes a fixed frame-storage pool, a restricted local Unix socket with
sealed read-only frame descriptors, NuvoStream framing, HTTPS snapshot and
WebSocket streaming over Ethernet or USB Ethernet, a C ABI, a Python SDK,
whole-frame recording/replay, mDNS discovery, and a synchronized web viewer.
The native gateway has a bounded client count, TLS 1.2 minimum, strict request
limits, same-origin checks, CSP, claim-token exchange, and Secure/HttpOnly
SameSite cookies. It has no camera-device access.

Capture collection runs independently of processing and publication. Pending
pairs are replaced as whole frames under overload, while consumer-held storage
remains immutable. The gateway receives raw frames from one local IPC source,
shares immutable serialized data between clients, and wakes streams on new
generations. Raw filesystem snapshots are an explicit diagnostic option.
Configuration writes use revision preconditions; matcher application advances
the frame configuration epoch and reports requested and applied revisions.

Calibration tooling solves chessboard and ChArUco observations, validates RMS and
rectified epipolar error, binds a bundle to unit/camera/preset identities,
hashes every asset, activates through a serialized fsync-backed transaction,
and can roll back. User and factory authority are separate; factory activation
cryptographically verifies an Ed25519 signature against a separately
provisioned public-key trust store. Candidate balanced, quality, and fast
presets are present, but only measured gates can change their qualification
labels.

Guided calibration sessions have bounded retention and capture storage,
serialized operations, and a committed pair manifest binding image hashes to
one capture identity. Activation verifies a restarted daemon against the selected
calibration hash and restores the prior selection after failure. Boot recovery
reconciles interrupted activation before the daemon loads calibration. Measured
calibration geometry is authoritative after identity and integrity validation.

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

The same allowlisted control plane now applies the optional monitoring encoder,
the full-resolution SGBM/StereoBM matcher, and stereo-locked exposure. Manual
mode keeps exposure and analogue gain constant on both cameras; shared-auto
mode meters both ISP luma planes and emits one bounded command. Raw product
delivery and monitoring encoding are independent, and raw-only is the shipping
default.

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

- Native manual and shared closed-loop auto exposure are implemented and report
  requested plus per-camera applied metadata. The current rig still needs the
  retained trigger/illumination flicker gate and final per-pair control-epoch
  qualification before auto mode is a customer default.
- The gateway enforces transport preference against the actual accepted local
  Ethernet or USB interface. Exact product/cadence subscriptions require current
  measured source and interface qualification; resource reservations and bounded
  session queues are connected to the serving path. Explicit engineering
  interfaces allow labeled best-effort streams. The gateway does not configure
  a host network manager, and shipping Ethernet/USB qualification still requires
  measurements on the target cohort.
- The web console is a functional claimed viewer, health display, diagnostic
  view, transport control, monitoring-encoder control, matcher/preset control,
  and manual/shared-auto exposure editor. Guided calibration creates, captures,
  solves, activates, and rolls back through the allowlisted control plane.
  Real target observations and per-unit calibration validation remain required.
- The quality matcher now uses full eight-direction SGBM HH with a
  validity-preserving median, and the provisional right rectification map
  composes a robust correction measured across three retained Pi captures.
  This removes the reproduced horizontal scanline failure in offline camera
  validation. The half-resolution HH path is retained as the smoother,
  lower-cost candidate. The deployed full-grid Quality/HH path was measured at
  2.84 depth frames/s (342.01 ms median compute) across 60 live frames. The
  corresponding aligned 60-frame trace again found brightness variation in the
  ISP planes while exposure and gains stayed fixed; XTR/lighting evidence is
  still a release gate, not an inferred pass.
  The browser now reads exact matcher candidates from
  `web/depth-profiles.json`; its balanced default agrees with `stereo.json` and
  the native YAML fallback. All catalog profiles have unqualified throughput,
  and the historical calibration ID does not imply 10 depth frames/s. Fast's
  narrower 64-pixel search and disabled median are explicit tradeoffs rather
  than an automatic quality-default change.
- Recording uses the bounded, versioned `NVR1` frame-set container. An MCAP
  exporter remains an extension point and is not advertised as implemented.
- The Python SDK records directly from the persistent authenticated WebSocket
  stream and deduplicates by device, run, epoch, and sequence. Latest-only H.264
  monitoring frames are independently decodable; encoding preflight skips early
  optional color copies without charging cadence to discarded capture pairs.

## Next hardware work

Add the nine required manual artifacts listed in
`docs/pi5-hil-qualification-20260909.md` to the retained qualification evidence
directory. Regenerate its report with `--resume --skip-automated`; stored
automated gate identities and hashes are verified before reuse. The generated
report remains the single release authority.
