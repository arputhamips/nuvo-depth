# Pi 5 software release-readiness report — 2026-09-14

## Outcome

The Pi 5 software is a usable engineering release candidate for synchronized
capture, GPU rectification, full-resolution SGBM disparity/depth, raw product
delivery, optional monitoring encoding, and browser configuration. It is not a
factory-calibrated release: per-unit calibration and the retained hardware and
lifecycle gates listed below remain release authority.

## Verified on the live device

Device: Raspberry Pi 5 with two externally triggered IMX296 cameras. Active
runtime: native C++ daemon, 728 × 544 capture/rectification/matching/depth,
60 Hz requested capture cadence, full-resolution SGBM, 96-pixel search,
7-pixel block, four OpenCV threads, manual 8,000 µs / 2× exposure, and EGL/GLES
rectification.

- Host build and tests: 42/42 passed.
- Pi production build and tests after the final live capture telemetry patch:
  43/43 passed.
- Native arm64 Debian packages were assembled successfully after the final
  changes; the package-time test gate also passed 43/43. The unsigned
  engineering cohort, build metadata, and SHA-256 manifest are retained under
  `deliverables/pi5-release-candidate-20260914/`.
- Native daemon, gateway, and privileged control socket active.
- Exposure update verified at 4,000 µs / 2×; both cameras reported 3,999 µs.
- Shared-auto mode verified, then the device was returned to constant manual
  exposure. Configuration and controller error counters remained zero.
- Stereo configuration was round-tripped through the authenticated API without
  restarting capture.
- `both` mode returned a fresh 4,753,430-byte NUV1 raw frame and a valid
  23,347-byte H.264 envelope. `raw` mode continued advancing capture and raw
  products while the encoded endpoint correctly returned HTTP 503.
- Every web-console page opened successfully. Raw Live viewer, monitoring
  controls, algorithm controls, exposure controls, diagnostics, and safe
  connection controls were exercised against the live gateway.

## Measured pipeline performance

Sixty distinct published frames were sampled in raw-only mode. These are live
health samples, not a synthetic replay:

| Stage | minimum | p50 | p95 | maximum | Hardware |
| --- | ---: | ---: | ---: | ---: | --- |
| Rectification | 7.850 ms | 8.673 ms | 10.257 ms | 11.452 ms | Pi 5 V3D via EGL/GLES |
| Stereo preparation | 0.138 ms | 0.168 ms | 0.267 ms | 0.296 ms | CPU |
| SGBM match | 120.720 ms | 140.911 ms | 145.541 ms | 147.220 ms | four CPU cores |
| Disparity postprocess | 0.868 ms | 1.020 ms | 1.286 ms | 1.963 ms | CPU |
| Complete compute | 134.122 ms | 153.940 ms | 159.612 ms | 160.586 ms | combined |

Median full-frame validity was 51.5%; median validity inside the searchable
region was 60.5%. The sampled compute ceiling is about 6.5 depth frames/s.
A separate five-second live counter check measured 58.45 trigger pulses/s,
35.19 completed stereo pairs/s, and 5.96 published depth frames/s, with zero
trigger deadlines, trigger errors, frame-completion errors, or requeue errors.
The paired-camera/ISP path and full-resolution matcher therefore use newest-frame
replacement under overload; this is not 60 Hz ISP-pair or depth output.

At the same operating point, `depthd` used about 327% aggregate CPU and 252 MiB
RSS. The four matcher workers used roughly 65–75% of one core each, libcamera's
camera manager about 14%, and its two IPA proxy workers about 4–5% each. Gateway
load while serving the local viewer was about 6.5% CPU and 39 MiB RSS. System
temperature was 73°C, CPU clock 2.4 GHz, V3D clock 960 MHz, and
`get_throttled=0x0`.

The optional 728 × 544 H.264 monitoring path previously measured about 4.6 ms
per encoded frame. Raspberry Pi 5 uses software video encoding, so leaving it
off remains the correct performance default. Rectification is already on the
GPU. Moving OpenCV SGBM to the Pi GPU is not an available production backend;
the supported performance levers are a narrower search, StereoBM with its
density tradeoff, or a future dedicated accelerator/backend.

## Shipping defaults and user flow

- Raw associated products on; monitoring encoder off.
- Balanced full-resolution SGBM, 96-pixel search, 7-pixel block, four threads.
- Constant stereo-locked manual exposure at 8,000 µs and 2× gain.
- Web changes use validated atomic control files and a root-owned allowlisted
  system agent.
- Encoder, raw output, and simultaneous modes are explicit independent toggles.
- Exposure page shows requested values and each camera's reported metadata.
- Presets page applies balanced or wide-search matcher settings; custom and
  StereoBM controls live on the Depth algorithm page.

## Remaining gates, in priority order

1. **P0 — Per-unit stereo calibration and metric acceptance.** The current seed
   is visibly provisional; this is the next planned phase.
2. **P0 — Trigger/electrical evidence and flicker closure.** The retained
   60-frame trace found brightness variation already in ISP luma, before
   rectification/encoding/network. Manual control prevents software AE drift,
   but illumination/XTR timing still needs scope evidence on the final rig.
3. **P0 — Clean-image package lifecycle gate.** The final arm64 Debian cohort
   builds cleanly and passes its package-time tests. Exercise install, upgrade,
   and rollback on a clean image before assigning production release authority;
   the live device currently uses a manually overlaid engineering build.
4. **P1 — Thermal and recovery endurance.** Complete the 72-hour soak and
   power-cut/filesystem recovery tests with retained logs.
5. **P1 — Wired transport qualification.** Measure sustained Gigabit Ethernet.
   Enable USB Ethernet only after power, VID/PID, descriptors, and target-host
   compatibility are approved. A 4.75 MB raw frame is bandwidth-heavy; USB 2
   must not be assumed adequate without measurement.
6. **P2 — Performance expansion.** Qualify a denser StereoBM preset or a
   dedicated stereo accelerator if more than roughly 6.5 full-resolution depth
   frames/s is required. Do not market a 60 Hz depth rate from the current CPU
   SGBM path.
