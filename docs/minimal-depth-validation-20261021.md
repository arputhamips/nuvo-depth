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

After the user switched on the lights, another three associated stereo previews
and 300-frame stream were captured. Median raw brightness rose to 51/255 left
and 55/255 right. Delivery was 9.99980 fps, with no missing/duplicate stream
frames or completion/requeue/trigger/processing errors. Frame age was 100.71 ms
median, 101.10 ms p95 and 101.70 ms maximum. Valid pixels ranged from 62.23% to
64.21%, with a median of 63.29%. Final temperature was 58.2 °C with no throttling.

The brighter scene confirmed a remaining vertical alignment discrepancy:
496 mutual SIFT correspondences, selected by a 0.7 descriptor ratio and positive
disparity below 128 px without a vertical-error filter, had 2.82 px median
absolute vertical error and 4.45 px p95. The beta 5 calibration remains selected
as requested; neither its matrices nor the measured depth were adjusted to
hide this residual. Stream throughput and numeric conversion pass, but current
rectification quality and absolute accuracy remain unqualified. No known
distance reference was supplied for this scene.

Local evidence and three preview images are retained under
`output/minimal-depth-validation-20261021-run2/`; complete raw frames and
metadata remain on the Pi under the same directory name in `/home/nuvo`.
The illuminated scene's evidence and three final previews are under
`output/minimal-depth-validation-20261021-lit/` and the corresponding Pi folder.

## Matcher comparison

The latest validated calibration recorded for beta 5 is selected. Its matcher
controls were separate from calibration: the earlier Pi used SGBM HH, a 96 px
range and a 5 × 5 validity-preserving median filter; the user configured minimal
with 3-way, 128 px and no median filter. Replaying the exact same illuminated
capture through the native pipeline with those two control sets preserved
identical rectified images. HH reduced visible horizontal streaks and increased
valid pixel coverage from 63.21% to 68.50%. This does not resolve the vertical
alignment residual shared by both matcher outputs.

A live 90-frame Pi test with the beta 5 matcher controls delivered 3.286 fps,
with frame age 377.67 ms median and 427.56 ms p95. Median valid coverage was
67.92%; completion, requeue, trigger and processing error deltas were all zero.
At a requested 10 Hz, capture sequence skips are expected with this slower
matcher: the bounded pipeline keeps processing recent captures. Temperature
was 56.5 °C and throttling was `0x0`. The original 3-way/128 control file was
restored byte for byte in a `finally` block, and three fresh frames confirmed
the restored settings applied. The Pi's calibration remains restored.

The exact-frame comparison image is
`output/minimal-depth-validation-20261021-lit/matcher-comparison.png`; the HH
live evidence is in `output/minimal-depth-validation-20261021-lit-hh/`.

## Repository checks

The configuration/OpenCV host build passed all 54 CTest checks after updating
the default geometry, its immutable asset manifest and relevant fixtures.
The earlier provisional geometry remains a fixture for its existing parser
and map-correction tests. Runtime capture, stereo and depth code are unchanged.

Replaying the exact captured mono pair through the native pipeline with the new
shared seed reproduced the Pi's left/right rectified images, disparity, depth
and validity arrays exactly, including NaNs. Changing the shared authority
marker did not change geometry or numeric output.

Both ARM64 package profiles built successfully from source
`fa158f570ca513a3e878c552507872e9de509d11` using the manual package workflow:

- Minimal: [Actions run 39686442512](https://github.com/arputhamips/nuvo-depth/actions/runs/39686442512),
  54/54 tests and installation into a fresh runtime-only Trixie container passed.
- Full: [Actions run 39686445958](https://github.com/arputhamips/nuvo-depth/actions/runs/39686445958),
  56/56 tests and the existing installation smoke check passed.

Downloaded packages were unpacked on the workstation and both packaged YAML
pairs matched the new default source bytes exactly. Both daemon configurations
contained the measured baseline and focal length. No installer was run on the
Pi, and published beta 6 assets remain unchanged. Later commits on the branch
only extend this validation report.

The subsequent physical-Pi Actions artifact installation, original-session trace,
beta 4 replay and fresh ChArUco validation are recorded in
[the alignment investigation](calibration-recording-investigation-20261021.md).
The artifact installation preserves the selected per-unit pair and settings,
and the isolated 120-frame stream sustains 10 Hz. Current board alignment fails
the original calibration limit with both current and earlier capture stacks;
the restored package default does not fix that discrepancy. No new calibration
or release was made.
