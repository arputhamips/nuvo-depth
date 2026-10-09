# Raspberry Pi 5 100 mm depth-only product profile

## Status

This repository now defines one default Raspberry Pi 5 product profile:
`balanced_728x544_10`. It is a standalone depth-camera configuration for two
hardware-synchronized IMX296 sensors with a nominal 100 mm optical baseline.

The software geometry now uses full-resolution matching. Hardware acceptance
and throughput qualification are still required on the final enclosure, and
the checked-in calibration is a provisional seed rather than a production
calibration. The historical `balanced_728x544_10` profile ID is retained for
calibration compatibility; its original half-resolution performance claim no
longer applies.

## Default candidate operating point

| Property | Value |
|---|---:|
| Board | Raspberry Pi 5 |
| Accelerator | None |
| Sensors | 2 x IMX296 |
| Nominal optical baseline | 0.100 m |
| Capture target | 728 x 544 at 10 Hz; depth rate unqualified |
| Disparity/depth matcher size | 728 x 544 |
| Matcher | OpenCV CPU SGBM or StereoBM; two-thread 3-way SGBM is the default |
| Full-resolution disparity interval | 0 to 128 px by default |
| Rectified focal length | 493.542046188 px |
| Preferred rectifier | Cached OpenCV CPU remap |
| Rectifier fallback | EGL/OpenGL DMA-BUF |
| Preview | Left JPEG at 2 Hz |
| Product role | Standalone depth camera |

The product package includes capture, stereo rectification and matching,
selected disparity/depth or camera images, diagnostics, and an independent preview.
Legacy compact obstacle output is disabled by default.
It includes no semantic inference, Hailo runtime, HEF model, or accelerator
service dependency.

## Geometry

Metric range follows `Z = fB / d`. With the nominal profile,
`fB = 49.3542046188 px*m`. A 2 m target therefore has about 24.68 px disparity,
while a 6 m target has about 8.23 px disparity. The 128 px default corresponds
to a nominal near range of about 0.386 m. The compact obstacle output keeps its more conservative 0.75 m
minimum.

The logical-left sensor is camera index 1 and the logical-right sensor is index
0 on the established Pi topology. The provisional right projection matrix uses
`Tx = -fB = -49.3542046188`.

## Calibration requirement

The files under `config/calibration/nuvo_depth_100mm/` preserve the intrinsics
and rectification seed from the existing camera unit and replace the projection
baseline with 100 mm. That is enough to establish the software contract, but it
does not account for the final sensors, lens focus, enclosure tolerances, or
actual rotation/translation of the assembled rig.

The provisional right-camera document also carries a measured
`rectified_output_correction`. It was robustly fitted across three retained
camera captures after the full-resolution matcher exposed a systematic vertical
residual: median absolute feature error fell from 1.77 px to 0.15 px and SGBM
coverage rose by about 4.2 percentage points. The correction composes the dense
right rectification map only; it does not change focal length, baseline, or the
metric depth equation. This empirical correction improves the engineering
preview but does not promote the seed to a validated calibration.

## Matcher qualification

The retained quality candidate selects full-resolution SGBM with
eight-direction `HH` aggregation, a 96-pixel range, 5-pixel block, uniqueness
0, and a 5 x 5 validity-preserving median. On three retained Pi captures this
reduced the measured low-texture horizontal-streak fraction from 11.1% with
3-way aggregation to 2.1%, while reducing the three-capture disparity standard
deviation p95 from 25.5 px to 8.4 px. Complete Sintel Clean and Final runs
covered 2,128 frames.

An offline lower-cost candidate is half-grid SGBM HH at
364 x 272 with 48 working disparities, restored to 728 x 544 as a 96-pixel
full-grid interval through invalid-aware upscaling. It retained 74.2% of the
tested camera grid, reduced three-capture instability to 3.17 px p95, and was
visually the cleanest result. It gives up full-grid edge detail and slightly
more Sintel accuracy, so it remains an explicit performance profile rather
than silently changing the quality output. It has no current live throughput
qualification and is not a browser-selectable profile. The retained
`nuvo_pi5_depth_100mm_halfres_benchmark.yaml` is a separate historical 3-way,
64-working-disparity benchmark, not this HH candidate.

The authoritative browser matcher catalog is `web/depth-profiles.json`. Its
default balanced controls agree with both `config/stereo.json` and the native
YAML fallback. Quality, balanced, fast, and wide are all unqualified candidates;
their labels carry no frame-rate guarantee. The retained full-grid Quality/HH
Pi run measured 342.01 ms median compute and 2.84 depth frames/s at a 60 Hz
capture target. It is historical evidence, not qualification for a revised
build or the default 10 Hz capture target. See
[performance profiles](performance-profiles.md) for exact controls and range tradeoffs.

Before reporting calibrated metric accuracy, capture a new stereo calibration
on the assembled 100 mm camera and replace both seed files. Confirm the measured
translation norm and projection-derived baseline agree with the mechanical
baseline, then regenerate the package manifest hashes.

## Hardware acceptance gates

Run these checks on the final Raspberry Pi image and enclosure:

1. Confirm both IMX296 devices, external-trigger mode, GPIO 17 idle state, and
   one frame per trigger pulse.
2. Verify logical left/right order and orientation from a captured raw pair.
3. Produce and review rectified calibration images; vertical residuals must be
   within the selected calibration acceptance threshold.
4. Run the packaged service for an extended soak and measure sustained depth
   rate and latency with the selected matcher. A requested 10 Hz depth service
   requires a measured 10 Hz pass; verify bounded pair skew, no camera lease
   leaks, and no repeated fallback/reset.
5. Measure depth error at several known distances across the field of view.
6. Verify the Debian package installs and starts without Hailo packages,
   services, devices, or model files present.

Historical benchmark documents in this repository describe the earlier rig.
They remain useful as compute-path evidence, but they are not metric-accuracy
evidence for the new 100 mm mechanical assembly.
