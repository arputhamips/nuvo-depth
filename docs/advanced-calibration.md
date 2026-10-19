# Advanced calibration profile production

Calibration is a separate manufacturing and engineering operation. It is not a
step in the normal Nuvo Depth user flow. Trained operators use **Calibration
profiles** in the browser console to capture, solve, qualify, save, activate,
and roll back geometry presets over Ethernet or Wi-Fi.

## Why Nuvo uses ChArUco

ChArUco is the default production target. Its coded markers remain identifiable
when part of the board is occluded, while the interpolated chessboard corners
provide subpixel precision. That is a better fit for a guided browser workflow
than an ordinary chessboard. AprilGrid is also a strong engineering target, but
supporting a second detector would add another profile and validation surface;
it is not required for the current IMX296 production flow.

The method follows the relevant practices in the
[OpenCV ChArUco guide](https://docs.opencv.org/5.0/tutorials/objdetect/charuco_detection/charuco_detection.html),
[Kalibr target guidance](https://github.com/ethz-asl/kalibr/wiki/calibration-targets),
[ROS camera-calibration workflow](https://docs.ros.org/en/kilted/p/camera_calibration/doc/tutorial_mono.html),
and the live-error conventions used by
[Stereolabs](https://docs.stereolabs.com/docs/integrations/opencv/zed-open-cv-calibration).

## Accuracy boundary

The H.264 stream on the page is only a low-latency positioning aid. Every
accepted calibration observation is a synchronized, unrectified, full-profile
luma pair captured locally from the two ISP outputs. Compression, network
latency, the existing rectification map, disparity, and depth are never inputs
to the solver.

This separation makes Wi-Fi safe for operating the workflow: packet delay can
affect what the operator sees, but not the captured stereo pair or its geometry.

## Prepare the target and rig

1. Mount a printed ChArUco board on rigid, flat material. Use a matte finish and
   a white border at least one square wide. Do not use a curled sheet of paper.
2. Measure the printed square and marker dimensions after mounting; printer
   scaling is not assumed to be exact. Enter the measured values in millimetres.
3. Measure camera optical-centre to optical-centre baseline. The default
   production tolerance is 2 mm, but a tighter independently verified
   measurement can use a tighter gate.
4. Lock both lenses mechanically. A focus or lens change invalidates the
   profile.
5. Select **Manual · constant** exposure, shared gain and colour gains. Avoid
   reflections, clipped highlights, motion blur, changing illumination, and
   independent auto-exposure during the series.
6. Confirm the stable left/right camera identities and the exact sensor readout,
   crop, orientation, and output dimensions.

For printed ArUco-family targets, keep the surrounding white margin larger than
roughly 70% of one marker module so the outer marker boundary remains detectable.

Calibration supports OpenCV 4.6 or newer with the ArUco Python bindings. It uses
the post-4.6 ChArUco printed pattern and records `layout_version: opencv-4.6` in
the session and solved bundle. Existing sessions without that field retain the
same post-4.6 interpretation. OpenCV changed the pattern for even-row boards;
targets printed using earlier versions are not accepted as that layout. Confirm
the printed target before reusing an older board. See the
[OpenCV pattern compatibility note](https://docs.opencv.org/4.10.0/df/d4a/tutorial_charuco_detection.html).
OpenCV 4.6 uses marker detection followed by ChArUco interpolation; newer bindings
use `CharucoDetector`. Both paths verify the board's marker placement and corner
coordinates, and a missing required capability produces an installation error.

## Browser workflow

Open **Advanced → Calibration profiles**.

### 1. Target and profile

Enter a preset name, sensor mode, image dimensions, measured baseline, board
geometry, and ArUco dictionary. The page takes the current width, height, and
frame rate from the running camera profile.

One geometry preset may list several frame rates only when the complete sensor
readout, binning, crop, orientation, ISP scaling, and rectified dimensions are
identical. Otherwise create a separate preset. Any change to optics, focus,
relative camera pose, or baseline also requires a new preset.

Start the session. Session state and raw evidence are durable on the Pi, so the
workflow can report an interrupted capture, solve, or activation and allow a
retry. Operations on one session are serialized. A capture is complete only
after the daemon commits a manifest containing the device/run UUID, capture
epoch and sequence, dimensions, and hashes of both PNGs and their metadata.
Partially written pairs are discarded and cannot enter the solver.
The browser can reconnect without losing accepted observations.

Control-API responses include only the latest observation in `captures`, with
`capture_history_total`, `capture_history_included`, and
`capture_history_truncated` describing the returned history. This keeps live
status and capture replies within the 64 KiB control-transport limit as a session
grows. `coverage.occupied_cell_indices` supplies the complete 3×3 coverage grid
independently of the returned observation list. Full observations, camera
metadata, and quality measurements remain in
the session's `status.json` and capture files on the device; the
`nuvo-calibration web-status --state-root /var/lib/nuvo-depth --session SESSION`
command returns the complete history.

Calibration evidence is bounded: at most 400 capture attempts and 512 MiB per
session, 16 retained sessions, and 2 GiB across the session store. Admission
reserves space before asking the daemon to capture, including in-flight writes.
Failed or cancelled attempts keep their conservative reservation charge until
the session is removed. A profile whose required capture set exceeds the quota
is rejected at creation. Finished inactive sessions are removed oldest first
when space is needed; inactive sessions expire after seven days, and unfinished
idle sessions after 24 hours. Active and busy sessions are retained. Run
`nuvo-calibration web-cleanup` to apply the same idle retention policy manually.

Before daemon startup, a recovery hook restores any interrupted calibration
selection from its durable transaction journal. During an intentional restart,
the live activation process holds the store lock, so the hook allows that
staged selection to load. A killed activator releases the lock and its
uncommitted selection is restored before the next startup.

### 2. Capture coverage

Enable the positioning preview and choose the left or right camera. The page
temporarily enables a low-latency H.264 monitoring configuration and remembers
the prior encoder settings. Keep the complete target inside the guide, hold it
still, then select **Capture synchronized raw pair**. Use **Restore stream
settings** when positioning is complete.

Collect at least 30 accepted pairs:

- fill the centre, all four corners, and all four edges;
- include near, middle, and far board sizes;
- use positive and negative pitch and yaw, plus a few roll angles;
- do not collect many almost-identical fronto-parallel views;
- keep the board sharp and visible in both cameras.

The page annotates both raw images and immediately marks the observation as
accepted or **retake**. The default per-pair checks are:

- at least 12 shared ChArUco corners;
- Laplacian sharpness of at least 40 in the target region;
- mean target luma between 15 and 240;
- at most 8% near-black or near-white clipped target pixels;
- a meaningfully different target centre, area, or pose from prior captures.

The 3 × 3 field indicator must reach the configured coverage score (65% by
default) as well as the accepted-pair count. Rejected frames remain as evidence
with their reason and never enter the solver.

### 3. Solve and inspect

The solve becomes available only when count and coverage gates pass. It:

1. detects labeled ChArUco corners and refines them at subpixel precision;
2. holds out every fifth varied observation for validation;
3. fits independent five-coefficient OpenCV plumb-bob intrinsics with
   `calibrateCameraExtended`;
4. removes high-error training views using a median/MAD robust rule and refits;
5. fits stereo rotation and translation with the intrinsics fixed;
6. computes rectification and vertical epipolar residual on training and
   previously unseen holdout observations;
7. compares solved baseline with the measured physical baseline.

The five-coefficient model is deliberate. A rational model is not enabled by
default because a planar target can over-parameterize those extra terms and
produce an apparently low training error with unstable geometry.

The default pass gates are:

| Metric | Default gate | Meaning |
| --- | ---: | --- |
| Left intrinsic RMS | ≤ 0.50 px | Left camera model fit |
| Right intrinsic RMS | ≤ 0.50 px | Right camera model fit |
| Stereo RMS | ≤ 0.50 px | Joint stereo model fit |
| Holdout vertical epipolar p95 | ≤ 0.50 px | Rectification on unseen poses |
| Solved baseline error | ≤ 2.00 mm | Scale sanity check |
| Field coverage | ≥ 65% | Spatial diversity gate |

The UI also shows worst epipolar residual, train/holdout counts, and removed
outliers. A geometric **PASS** means the model and baseline passed their gates;
it does not by itself certify absolute depth accuracy.

### 4. Save, activate, or roll back

Solving always saves an immutable, content-addressed preset and its evidence.
A failed result is retained for diagnosis but cannot be activated. Activating a
passing preset validates its manifest and asset hashes, atomically changes the
user-calibration pointer, and restarts the native depth service. **Roll back**
atomically restores the previous validated user preset.

After activation, confirm the reported calibration hash equals the selected
bundle ID, then inspect rectified imagery and disparity before qualification.

## What the reported errors mean

- **Intrinsic RMS** measures average target-corner reprojection fit for one
  camera. It does not test the opposite camera or real-world depth scale.
- **Stereo RMS** measures the joint fit of both lens models and the stereo pose.
- **Holdout epipolar p95** is the 95th-percentile vertical mismatch after
  rectification on observations excluded from fitting. It is the most useful
  browser metric for catching geometry that only fit the training poses.
- **Worst epipolar error** exposes a bad tail that a mean can hide.
- **Baseline error** checks physical scale but is only as accurate as the board
  dimensions and manual baseline measurement.

The browser solver refines image corners and jointly fits the stereo geometry.
It compares the five-coefficient Brown model with an eight-coefficient rational
model using training data only and penalizes the extra coefficients. A model
must invert across the complete sensor field, have no radial fold or denominator
pole in that domain, and have finite, orientation-preserving sampled rectification
maps before it can be selected. Held-out observations remain excluded from fitting
and selection. Validation uses a converged inverse rather than OpenCV's default
five iterations. All existing accuracy gates still apply.

The capture profile ID comes from the running device and is read-only in the
form. Activation binds the result to that profile and the stable camera identities.
Re-solving after adding views preserves the existing holdout IDs; an earlier
training fit may initialize the optimizer, but holdout points never initialize
or select its lens model. A folded compact model cannot reject edge observations
as outliers; those views remain available to the wider model.

Rational bundles identify their model as `rational_polynomial` and contain exactly
eight coefficients. The runtime computes rectification maps when loading a bundle;
the coefficient count does not add a lens-model evaluation to each captured frame.

For comparison, Stereolabs describes reprojection error below 0.5 px as good,
0.5–1 px as acceptable, and above 1 px as poor. Nuvo therefore uses 0.5 px as
the default release gate rather than an acceptance ceiling of 1 px.

## Production acceptance after a geometric pass

Before releasing a unit, independently verify:

- depth scale and bias at several surveyed distances across the advertised
  range and image field;
- plane flatness, edge behaviour, invalid fraction, and disparity consistency;
- synchronization, exposure stability, and accuracy at every advertised frame
  rate;
- repeatability after restart and thermal soak;
- the active bundle ID and stable camera identities in the release record.

Factory activation remains separate and requires an Ed25519-signed manifest
whose key is present in the factory trust store. The browser creates and manages
the qualified user namespace; it cannot manufacture a factory signature.

## Troubleshooting capture failures

| Message or symptom | Corrective action |
| --- | --- |
| Not enough shared corners | Move closer, show the whole board in both cameras, verify dictionary and board dimensions |
| Image is soft | Shorten exposure, improve light, hold the board still, and recheck lens focus |
| Exposure or clipping rejection | Remove glare and use shared manual exposure/gain |
| Duplicate pose | Move to another field cell, distance, or tilt |
| Coverage incomplete | Fill the unlit cells in the 3 × 3 coverage guide |
| Low training RMS but poor holdout p95 | Add more varied tilts/distances and remove warped-board or blurred captures |
| Baseline gate fails | Re-measure the rig and printed board; check left/right identity and target flatness |

## Command-line and matrix workflow

The browser workflow is preferred for one physical unit. Batch manufacturing
and reproducible profile matrices can use the same installed solver:

```bash
cp /usr/share/nuvo-depth/calibration/pi5-profile-matrix.example.json \
  ./calibration-plan.json

/usr/lib/nuvo-depth/nuvo-calibration matrix ./calibration-plan.json \
  --output-root ./calibration-results
```

The matrix command validates identity, geometry, frame-rate reuse, dimensions,
pair count, RMS, and epipolar thresholds. It writes a reviewable result even on
failure, and failed bundles cannot be activated.

Validate and activate a finished command-line bundle with:

```bash
/usr/lib/nuvo-depth/nuvo-calibration validate ./calibration-results/PRESET
sudo /usr/lib/nuvo-depth/nuvo-calibration activate \
  ./calibration-results/PRESET \
  --state-root /var/lib/nuvo-depth --namespace user
```
