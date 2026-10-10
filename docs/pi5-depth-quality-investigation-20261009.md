# Pi 5 depth quality investigation — 2026-10-09

The device still uses authority-0 provisional geometry. The owner confirmed
that this hardware has never had a good calibration and that neither camera,
mount nor focus changed during the reported deterioration.

## Findings from live captures

- Independent OpenCV 4.10 replay of the captured pair reproduced both CPU
  rectified images exactly: zero differing pixels. It also reproduced the
  live SGBM validity support and every valid disparity exactly. This rules out
  a discrepancy in the deployed matching or CPU rectification implementation
  for the tested configuration and pair; it does not certify geometric quality.
- Libcamera's sensor discovery order changed across reboot. Index-based camera
  selection therefore exchanged the physical left and right sensors while
  retaining their calibration maps. Camera selection now sorts stable camera
  IDs and resolves the selected ID through CameraManager. The installed fix
  retained left `i2c@88000` and right `i2c@80000` after another reboot.
- Before that fix, scene feature matches had approximately 38.4 px median
  absolute vertical mismatch. Four fresh captures after the fix measured
  13.41–13.48 px median and 22.90–23.91 px p95. These scene-feature diagnostics
  are not a calibration qualification test, but show a large remaining error.
  Production qualification requires holdout target epipolar p95 ≤ 0.50 px.
- The seed calibration contents and SHA-256 remained unchanged during the
  reinstall. No calibration was solved or activated during this investigation.
  An offline scene-based alignment experiment remains diagnostic only.
- The owner's live controls during capture were 3-way SGBM, two threads,
  block size 7, 96 searched disparities, minimum 24, output maximum 64,
  uniqueness 10 and no median filter. The minimum/maximum limits exclude
  depths outside their accepted disparity interval. These controls were
  preserved during deployment.

## Performance and next step

On a frozen 728 × 544 live pair, two-thread 3-way SGBM with 128 disparities
measured about 72 ms per match without median filtering and about 79 ms with
uniqueness 5 in the median-3 candidate. The matching measurements exclude the
separate median postprocess, rectification, capture and delivery costs. Full
HH took about 379 ms per match, so it cannot satisfy the requested 10 Hz profile.
Changing penalties or smoothing cannot repair the rig's vertical mismatch.

Use **Calibration profiles** to capture varied poses of a measured, rigid
ChArUco target, solve and qualify a profile for these physical camera IDs and
the operating image geometry, then activate a passing profile. Follow
[the calibration workflow](advanced-calibration.md), including held-out poses
and independent baseline measurement. Tune the 10 Hz SGBM profile afterward.

## Raw delivery recovery

An idle native source retains its last frame. The gateway rejected an old
cached capture by throwing, then disconnected, reset its cursor and fetched
the same old capture again. Demand was repeatedly withdrawn before the native
capture loop could produce a fresh pair. The gateway now drops that capture
while retaining the subscription and advanced cursor; stale frames remain
ineligible for delivery. A regression test models delayed producer wake-up.

Raw captures, replay measurements and deployment receipts are retained locally
under `output/pi5-quality-20261009/`, including the `after-binding/` reports.
