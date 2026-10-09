# Calibration UI and Pi 5 validation — 2026-09-16

## Outcome

The advanced ChArUco calibration workflow is implemented, packaged, installed,
and live-tested on the production Raspberry Pi 5. It supports browser-guided
profile setup, encoded positioning assistance, synchronized raw ISP capture,
automatic capture qualification, robust solve and holdout validation,
content-addressed preset saving, activation, and rollback.

The unit was not geometrically calibrated in this run because no ChArUco target
was present in the camera field. The workflow correctly rejected the test pair
and did not enable solving or activation. A real accuracy result requires the
operator to place the measured board and collect the required varied poses.

## Method selected

- ChArUco is the production target: coded partial-view recovery plus subpixel
  chessboard-corner accuracy.
- The H.264 feed is a positioning aid only.
- Solver inputs are synchronized, unrectified, full-profile ISP luma images
  captured locally on the Pi.
- Each fifth varied capture is held out from fitting for an unseen-pose
  rectification check.
- Intrinsics use the stable five-coefficient OpenCV plumb-bob model.
- High-error training views are removed using median/MAD rejection before the
  fixed-intrinsic stereo fit.
- A preset passes only if intrinsic RMS, stereo RMS, holdout vertical epipolar
  p95, baseline scale, and field coverage all pass.

The complete operator method and metric definitions are in
[advanced-calibration.md](advanced-calibration.md).

## Browser hardware test

Tested through the real authenticated gateway and Pi calibration agent:

1. The page populated the active 728 × 544, 10 Hz camera profile.
2. A session was created and persisted on the Pi.
3. The temporary H.264 positioning aid started at 728 × 544.
4. Left/right selection changed the encoded source without a page reload.
5. Restore returned the previous encoder configuration.
6. A synchronized raw pair was captured from both ISPs.
7. With no target present, the pair was marked **retake** with 0 shared
   corners and the reason `not enough ChArUco corners are visible in both cameras`.
8. Both annotated 728 × 544 raw captures were served by the preview endpoints.
9. The accepted count remained 0/30, rejected count became 1, and solve stayed
   disabled.
10. Light/dark appearance and session reconnection worked with no browser
    console errors.

## Solver and package tests

The final arm64 production build passed 44/44 tests on the Pi in 13.62 seconds.
Coverage includes:

- synthetic ChArUco stereo recovery at a known 100 mm baseline;
- calibration activation, rollback, manifest, and signature behavior;
- system-agent command and injection allow-list tests;
- authenticated gateway calibration integration;
- web UI contract and unique IDs;
- encoder, exposure control, rectification, SGBM, validity, SDK, IPC, and
  product-envelope tests.

The synthetic test is a deterministic algorithm check, not a substitute for a
physical target and real-lens calibration.

## Live runtime after installation

| Item | Observed |
| --- | --- |
| Image/depth dimensions | 728 × 544 / 728 × 544 |
| Capture target | 10 Hz |
| Rectification | EGL/GLES DMA-BUF path, 7.91 ms sample |
| Stereo | OpenCV CPU SGBM, 298.08 ms match sample |
| Total processing | 312.20 ms sample |
| Searchable / output valid fraction | 56.7% / 48.7% sample |
| Exposure | Manual, 7,999 µs reported on both cameras |
| Analogue gain | 1.995× reported on both cameras |
| Services | Target, daemon, gateway, and agent socket active |
| Service restarts | 0 daemon / 0 gateway |
| Temperature after build/install | 64.2 °C |
| Throttling | `0x0` |

The full-resolution quality matcher is deliberately CPU-heavy; this sample is
about 3.2 processed depth frames per second even though capture is triggered at
10 Hz. Calibration raw capture remains synchronized and independent of matcher
throughput.

## Final package artifacts on the Pi

| Package | SHA-256 |
| --- | --- |
| `nuvo-depth-core_0.1.0_arm64.deb` | `2f61b081538649732fdd2e89cc8442bbd7c018f64e6c79e5378fe0cdf5471f26` |
| `nuvo-depth-gateway_0.1.0_arm64.deb` | `a3db8795ef209acc1f15a1aff13264f252809d5669fa0753d12412863843765d` |
| `nuvo-depth-calibration_0.1.0_all.deb` | `07e9a911ba1302709eb2ad2904001330ea1528064e61c410949cc3e09d1ec8fe` |
| `nuvo-depth-tools_0.1.0_all.deb` | `0a106c2757171b2318618c9918f29229ce4594567d24cd6992ec0571d6afba51` |

Build workspace on the Pi:
`/home/nuvo/nuvo-depth-calibration-ui-keMrbuzP`.

## Remaining physical acceptance step

Mount and measure the real rigid ChArUco board, lock focus and manual exposure,
then collect at least 30 accepted pairs across the field, distances, and tilts.
After a geometric pass, verify known-distance depth scale/bias, plane flatness,
invalid fraction, edge behavior, restart repeatability, thermal soak, and each
advertised frame rate before promoting the preset to factory release.
