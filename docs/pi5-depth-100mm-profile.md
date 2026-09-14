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

## Locked operating point

| Property | Value |
|---|---:|
| Board | Raspberry Pi 5 |
| Accelerator | None |
| Sensors | 2 x IMX296 |
| Nominal optical baseline | 0.100 m |
| Capture target | 728 x 544 at 10 Hz; depth rate unqualified |
| Disparity/depth matcher size | 728 x 544 |
| Matcher | OpenCV CPU SGBM or StereoBM, 128 default disparities |
| Full-resolution disparity interval | 0 to 128 px |
| Rectified focal length | 493.542046188 px |
| Preferred rectifier | EGL/OpenGL DMA-BUF |
| Rectifier fallback | OpenCV CPU |
| Preview | Left JPEG at 2 Hz |
| Product role | Standalone depth camera |

The product package includes capture, stereo rectification and matching,
disparity/camera metadata, diagnostics, preview, and compact obstacle output.
It includes no semantic inference, Hailo runtime, HEF model, or accelerator
service dependency.

## Geometry

Metric range follows `Z = fB / d`. With the nominal profile,
`fB = 49.3542046188 px*m`. A 2 m target therefore has about 24.68 px disparity,
while a 6 m target has about 8.23 px disparity. The configured 128 px maximum
corresponds to a nominal near range of about 0.386 m; the compact obstacle
output keeps its more conservative 0.75 m minimum.

The logical-left sensor is camera index 1 and the logical-right sensor is index
0 on the established Pi topology. The provisional right projection matrix uses
`Tx = -fB = -49.3542046188`.

## Calibration requirement

The files under `config/calibration/nuvo_depth_100mm/` preserve the intrinsics
and rectification seed from the existing camera unit and replace the projection
baseline with 100 mm. That is enough to establish the software contract, but it
does not account for the final sensors, lens focus, enclosure tolerances, or
actual rotation/translation of the assembled rig.

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
4. Run the packaged service for an extended soak and verify 10 Hz disparity,
   bounded pair skew, no camera lease leaks, and no repeated fallback/reset.
5. Measure depth error at several known distances across the field of view.
6. Verify the Debian package installs and starts without Hailo packages,
   services, devices, or model files present.

Historical benchmark documents in this repository describe the earlier rig.
They remain useful as compute-path evidence, but they are not metric-accuracy
evidence for the new 100 mm mechanical assembly.
