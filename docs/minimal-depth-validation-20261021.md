# Minimal beta 6 depth validation — 21 October 2026

The user-installed `nuvo-depth-minimal` 0.1.1~beta.6 was tested on NuvoMow,
Raspberry Pi 5, ARM64 Trixie, kernel `6.12.75+rpt-rpi-2712`. No installer or
additional runtime packages were run. Tests used the installed native pipeline
and its existing local frame socket; preview rendering ran on the workstation.

## Recovered calibration

Beta 5 qualification recorded active bundle
`8adf4c8103f1fb7f2811a7d483669cc99c8c7132902700ebb3fd464d19158cf2`.
Both YAML files, their manifest and validation record were recovered from the
retained 18 October calibration evidence and checked against manifest hashes:

| Asset | SHA-256 |
|---|---|
| front_left.yaml | `98518840e1c5d061a15c75a23997019f7ec4e88ca677a22acde62e75fc08230a` |
| front_right.yaml | `8d241f274918670713535542ce3553f4da1b282baeb8ba62d03db5ac9a11f503` |

The physical camera IDs, image dimensions and capture profile matched the
recovered manifest. The geometry is 728 × 544, baseline 0.0996937500869897 m,
rectified focal length 441.79166212006197 px and right projection translation
−44.04386755391325 px·m. Both cameras use rational distortion with eight
coefficients and no empirical output correction.

The exact user-validated YAML pair is now under `/etc/nuvo-depth/calibration`
on the Pi, selected by a persistent systemd drop-in. The daemon YAML baseline,
focal length and authority agree. The post-wipe device UUID was retained.
Because minimal loads the YAML pair directly instead of its old unit-bound
manifest, the runtime pair hash is
`41f14f56036d34b0a972e5b72dd1fd9cc6783862ca1da42313e86cd776f61d10`.
This differs from the active-bundle ID but contains byte-identical geometry.
The previous configuration is saved in
`/home/nuvo/minimal-before-beta5-calibration-20261021`.

The repository default now uses this geometry in both package profiles. Only
the shared seed's authority marker differs from the recovered YAML: it remains
provisional for other assembled camera units. `profile.json` records both
original and seed hashes, and a test reconstructs the exact original YAML by
changing that marker back. Published beta 6 assets were not replaced.

## Actual output

The CLI streamed 300 consecutive fresh depth/validity frames:

| Measurement | Result |
|---|---:|
| Dimensions | 728 × 544 |
| Capture timestamp cadence | 9.99952 fps |
| Delivery cadence | 9.99942 fps |
| Frame age, median / p95 / maximum | 100.68 / 101.19 / 101.73 ms |
| Valid pixels, minimum / median / maximum | 67.34% / 70.14% / 72.14% |
| Missing or duplicate frames within the stream | 0 |
| Completion / requeue / trigger / processing errors | 0 |
| Throttling | `0x0` |
| Temperature at end | 60.4 °C |

Every valid pixel was finite and positive; every invalid depth was NaN. Three
associated stereo/disparity/depth previews agreed with `Z = fB / d` within
0.00000763 m numerical error. This verifies conversion consistency, not
absolute scene distance. A separate `nuvoctl depth capture` also wrote the
float32 file, validity mask and JSON sidecar successfully.

The first 300-frame run also completed, but the harness initially treated a
five-count increase in `capture_sequence_gaps` as a failure. Inspection of
`apps/depthd_main.cpp` showed this counter measures gaps between published
captures, including intentionally unpublished captures between reader demand
changes. The repeat retained this counter separately: it increased by five
across the preview-to-stream transition while the 300 delivered frames were
consecutive, with no capture completion errors. No runtime change was needed.

## Preview quality limits

The scene was very dark: median raw brightness was 2/255 on both cameras,
at 8 ms exposure and analogue gain 2. The depth previews show horizontal
streaks and unstable regions with the default 3-way matcher. A small set of
24 mutually matched SIFT features had 3.01 px median and 3.54 px p95 vertical
residual; this is insufficient evidence of acceptable current rectification.
The same calibration had previously passed its retained target-based checks.
A well-lit textured scene and a measured distance reference are still needed
to assess current image quality and absolute range. No recalibration was
performed and no new accuracy qualification is claimed.

Local evidence and three preview images are retained under
`output/minimal-depth-validation-20261021-run2/`; complete raw frames and
metadata remain on the Pi under the same directory name in `/home/nuvo`.

## Repository checks

The configuration/OpenCV host build passed all 54 CTest checks after updating
the default geometry, its immutable asset manifest and relevant fixtures.
The earlier provisional geometry remains a fixture for its existing parser
and map-correction tests. Runtime capture, stereo and depth code are unchanged.
