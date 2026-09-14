# Raspberry Pi 5 hardware-in-loop qualification — 2026-09-09

## Outcome

The packaged Pi 5 beta software path passed every automated gate on the connected
dual-IMX296 rig. This is a hardware-backed beta result, not a production release
authorization: the checked-in calibration is provisional and nine physical,
metrology, endurance, and recovery evidence gates remain pending.

The authoritative report is
`deliverables/pi5-rig-20260909/contents/hil-20260909/installed-qualification-v2/qualification.json`.
It records `status: pending` and `release_authorized: false`.

## Tested cohort

- Raspberry Pi 5 Model B Rev 1.1, 16 GiB, AArch64, Debian 13 (trixie).
- Linux `6.18.33+rpt-rpi-2712`.
- `rpicam-apps` 1.12.0, `libcamera` 0.7.2+rpt20260817, and `libpisp` 1.7.0.
- Two IMX296 sensors at stable libcamera paths on the Pi 5 RP1 camera interfaces.
- IMX296 external-trigger driver mode 1 and GPIO 17 userspace trigger ownership.
- Candidate profile `balanced_728x544_10`: 728 x 544 rectified images,
  364 x 272 disparity/depth/confidence/validity, target 10 Hz.
- Debian cohort version 0.1.0, built natively on the rig with production Pi
  capture and EGL/GLES DMA-BUF rectification enabled; semantic/Hailo targets off.

## Automated gates

| Gate | Result | Hardware result |
|---|---:|---|
| Configuration | Pass | Installed `/etc/nuvo-depth/daemon.yaml` accepted; provisional authority remained explicit. |
| Paired capture | Pass | Three restart cycles, 1,000/1,000 pairs each; epochs 2/4/6; maximum sensor skew 29/27/30 µs; zero rejected, stale, or capacity-dropped events; zero callback/requeue errors; zero leases after stop. |
| Runtime directories | Pass | Package-owned tmpfiles policy created the standalone qualification runtime safely. |
| Native pipeline | Pass | 1,000 frames published in 100.986 s; capture sequence 1,000; zero sequence gaps; zero leases after stop. |

The final frame used `egl_gl_dmabuf` rectification with no fallback and
`opencv_sgbm_cpu` matching. Its measured stage times were 4.480 ms
rectification, 28.897 ms stereo, and 35.101 ms total. The associated NuvoStream
payload was 1,782,144 bytes and contained rectified left/right, disparity,
optical-axis depth, confidence, and validity products under one capture identity.

## Installed appliance checks

- All 39 ARM64 CTest cases passed during the final Debian build.
- APT replaced the inactive legacy Hailo-era `nuvo-depthd` package through an
  explicit bounded conflict/replacement rule. Its immutable asset release was
  retained as `previous`; the depth-only 100 mm release became `current`.
- Package installation did not enable or start appliance services.
- A transient start brought up the restricted native daemon, HTTPS gateway,
  system-agent socket, and web console without restarts or warning/error journal
  entries.
- The installed Python SDK authenticated with the device certificate and claim
  token, decoded a complete live frame, and observed all six advertised products.
  Anonymous API access returned 401 while the static claim page returned 200.
- The browser/API transport transaction selected and confirmed `usb`, then
  selected and confirmed `ethernet`; the final preference is Ethernet. This
  validates the policy and rollback path only—it does not qualify a physical USB
  gadget or Ethernet cable.
- A 3,346-frame transient service run had zero restarts, no warning/error journal
  entries, no throttling, and a 58.2 °C endpoint temperature. This is useful smoke
  evidence, not the required thermal soak.
- The final finite tmpfs run ended at 52.7 °C with `throttled=0x0`.

## Hardware-driven corrections made

The rig exposed and verified fixes for startup priming contamination, EGL context
ownership across scheduler threads, ARM64 multiarch library installation, Ninja
debhelper integration, migration from the legacy package, modern Raspberry Pi OS
boot-config discovery, least-privilege identity access, gateway-readable transport
state, standalone qualification runtime setup, and explicit published/gap health
counters.

## Pending release gates

The following artifacts are still required and must not be inferred from the
automated run:

1. Electrical design approval.
2. Oscilloscope evidence for trigger waveform and sensor exposure timing.
3. Per-unit stereo calibration bound to this unit, both camera identities, and
   the selected resolution preset.
4. Calibrated depth accuracy and valid-coverage measurements across the declared
   range.
5. Wired Ethernet TLS throughput and loss testing.
6. Approved USB-C power/data arrangement, VID/PID, host compatibility, and TLS
   throughput testing.
7. The required thermal/endurance soak.
8. Full clean-install, upgrade, downgrade, removal, and state-preservation package
   lifecycle testing.
9. Controlled power-cut and recovery testing.

Until those artifacts pass, keep the target disabled on customer images, retain
the explicit provisional-depth label, and do not publish the APT repository as a
production channel.
