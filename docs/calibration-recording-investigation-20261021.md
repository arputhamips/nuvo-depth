# Calibration comparison with the 20 October recording

The user supplied `preview-14s.png` from the 20 October web comparison and
reported that today's output was visibly worse. The screenshot shows HH SGBM
at 728 × 544 and approximately 2.5 fps. The earlier claim that the restored
profile was the relevant one had been based on beta 5 qualification, without
first tracing this recording. This investigation checks that link directly.

## Recording provenance

The workstation retains the original recording session under
`C:/Users/Workstation/Videos/Nuvo Vision One/2026-10-20/`. Both original NVR
files in `takes/take-20261020T192803Z/` passed their saved SHA-256 checks.
Both streams record calibration bundle
`8adf4c8103f1fb7f2811a7d483669cc99c8c7132902700ebb3fd464d19158cf2`,
with user-validated authority. The raw arrays retain the measured projection
product `fB = 44.04386755391325 px·m`.

The lossless 15-second take and the later browser screenshot are distinct
captures. The take manifest records 3-way SGBM, a 96 px search range, median 5,
four OpenCV threads, 8 ms exposure and gain 3. The supplied screenshot instead
shows HH. The comparison must not assign the take's 3-way setting to that
screenshot. The "Research first-time setup flow" chat was consulted and
independently confirmed the archived bundle and this distinction; it found no
empirical output correction in the selected YAMLs.

The earlier user-authorized Nuvo removal included the Pi's old calibration
archives, application backups and demo recordings. Inspection now found an
empty `/home/nuvo/Videos`, no old calibration state under `/var/lib/nuvo-depth`,
and the restored pair under `/etc/nuvo-depth/calibration`. The workstation
calibration archive and original recordings remain intact.

## Exact geometry and implementation checks

For recorded capture 5534, the original unrectified left RGB image was remapped
with the recovered YAML matrices and the same fixed-point OpenCV remap path.
The result was **byte-identical** to the rectified RGB image in the recording:
zero differing pixels and zero intensity error. This directly confirms the
restored left-camera rectification used for the original recording. Both
recovered YAML file hashes also match the original bundle manifest.

Historical raw stereo pairs were remapped with the same pair and checked by
mutual SIFT matches, using a 0.7 descriptor ratio and positive disparity below
128 px, without selecting by vertical error:

| Retained capture | Matches | Median vertical error | p95 vertical error |
|---|---:|---:|---:|
| 248d134276b843ca | 1,076 | 0.094 px | 0.604 px |
| e9763e44053e4e4a | 1,060 | 0.115 px | 0.564 px |
| 715b5003204143f7 | 1,009 | 0.153 px | 0.569 px |
| Known-distance scene, frame 0 | 748 | 0.193 px | 0.638 px |

There are no source differences between the recording's `c5eef0b` runtime and
beta 6 in the calibration store, Pi capture backend, main native daemon,
runtime factory or OpenCV rectifier.

The released full beta 5 daemon was then extracted without installation and
run on the Pi in file replay mode against the exact same new mono pair as
the installed minimal beta 6 daemon. The resulting left/right rectified
images, disparity, depth and validity arrays were **identical**, including
invalid NaNs. This rules out a full-versus-minimal implementation difference
for that controlled input.

## Live old-runtime check

The full beta 5 binary was temporarily selected through a runtime-only
systemd override, with the restored pair, HH/96/median 5, four threads and
gain 3 at 8 ms. No package or installer was run. Thirty frames delivered at
3.291 fps, with zero completion, requeue, trigger or processing errors.
The current stereo images still had 2.70 px median vertical residual and
4.45 px p95 over 659 matched features. Median valid coverage was 66.45%.

Thus the discrepancy also occurs with the older full runtime. The retained
calibration is correct for the earlier captures, but today's live image
alignment is inconsistent with it. The reason for that current alignment
change is not established. These checks do not prove a physical change,
and no new calibration or fitted correction was applied.

The temporary override was removed in a `finally` block; the minimal daemon,
original stereo controls and original exposure controls were restored, and
three fresh depth frames confirmed that restoration. The restored calibration
remains selected on the Pi.

## Display comparison

The first test previews used a different palette and a shorter display range
than the original console. A new comparison uses the original console's
blue-near/red-far palette and fixed 0.2–12.2 m scale. It still shows lower
quality today; recolouring does not resolve the alignment discrepancy.
The two images come from different captures and viewpoints and are not a
controlled scene comparison.

Local evidence is in `output/calibration-recording-investigation-20261021/`,
including `recorded-map-verification.json`, `historical-alignment.json`,
`runtime-array-comparison.json` and `recording-style-comparison.png`.
The old-runtime live evidence is in
`output/calibration-beta5-live-comparison-20261021/` and the matching Pi folder.
The package-default PR remains a draft; no release assets were replaced.

## Original board session, including the beta 4 check

The original session was located in the retained history of **Evaluate Nuvo
Depth Web UI**, rather than inferred from a release. The board has 13 × 9
squares, measured square size 34 mm, marker size 25.3 mm and dictionary
`DICT_5X5_100`. Session `c0c39e391fea4edd93e97c6607cf148f` was qualified
and activated on 18 October around 19:14 UTC. Its preserved outputs are in
`output/readiness-20261018/calibration-second/qualified-runtime/bundle/`.
The user subsequently accepted the 979 mm reading against the 1,008 mm laser
reference. This is the recovered measured pair, independently of beta labels.

The 20 October recording reused its `8adf4c…` identity. No separate completed
20 October calibration was found in the retained repo session histories; the
20 October calibration screenshot shows a new session at 0/30 captures. The
Research first-time setup flow chat independently confirmed this provenance.
It found no separate later right-camera file or independent asset hash from
that day. Because the native loader does not recompute a bundle ID from its
manifest, the recorded ID alone cannot exclude an undocumented file-and-manifest
edit. The exact original pair remains the best recovered evidence; the exact
recorded left map is independently verified above.

Beta 4's packaged seed has the earlier provisional geometry, rather than the
measured per-unit pair. Extracting that seed would not recover the user's
calibration. The released beta 4 binary was also run on the Pi against the same
new board input, recovered measured pair and HH/96/median-5 settings as the
Actions minimal binary. Rectified left/right, disparity, depth (including NaNs)
and validity arrays were identical. No beta 4 package was installed.
The report is `output/calibration-recording-investigation-20261021/beta4-runtime-comparison.json`.

## Fresh board validation

The user placed the original board in both cameras for a fresh stationary
check. Three synchronized pairs were saved with unmodified ISP luma and
rectified images. ChArUco IDs establish correspondence; corners use the same
3 px refinement window, 100 iterations and 0.0001 px termination as the original
fit. Coordinates are transformed through the unchanged recovered matrices
using iterative distortion inversion. No matches are filtered by vertical
error and no correction is fitted.

Each pair has 76 shared corners. Their p95 vertical errors are **3.061**, **3.074**
and **3.032 px**; all fail the original 0.50 px p95 limit. The first pair has
median absolute vertical error **2.566 px** and maximum **3.227 px**.
Evidence and annotated stereo images are in
`output/calibration-board-check-20261021/`.

The Pi's apt history records a camera package upgrade on 19 October from
`0.7.2+rpt20260817-1` to `0.7.2+rpt20261009-1`, between calibration and recording.
To check this hypothesis without downgrading the OS, the earlier Raspberry Pi
camera libraries, IPA and tuning files were extracted into a diagnostic folder.
The current binary is ABI-incompatible with that package, so a retained native
capture binary built before that upgrade was used for three finite raw ISP
traces. Its provisional seed was used only to satisfy the old binary's model
parser; none of its rectified or depth products enter this comparison. Its raw
left/right inputs were remapped offline with the recovered measured pair.

The earlier stack's new board pair has 82 shared corners, median **2.541 px**,
p95 **3.096 px**, maximum **3.286 px**. Both stacks selected full 1456 × 1088
sensor readout and produced 728 × 544 YUV420 images at rotation 180°. This check
does not identify the library upgrade as the explanation. The earlier binary
and libraries were never installed as packages. The Actions runtime and system
camera libraries were restored after each attempt. The evidence is in
`output/calibration-pre-camera-upgrade-20261021/`.

The recovered calibration therefore fails current live board validation even
through the earlier capture stack. Its files are preserved unchanged. No
software alignment fix or new calibration has been applied, and the cause of
the geometry discrepancy remains unresolved. A new stereo calibration is
recommended before accepting current depth accuracy; the recovered pair should
remain available for rollback and comparison. A package-default change alone
must not be described as fixing this live alignment failure.

## Actions package installed and checked on the Pi

The minimal artifact from [run 39686442512](https://github.com/arputhamips/nuvo-depth/actions/runs/39686442512),
source `fa158f570ca513a3e878c552507872e9de509d11`, was installed directly as a
local Debian package. No bootstrap installer or release operation was run.
Package SHA-256 is
`40e06e9b17c319aa2a8b9efc48a01e4704243ed4bfd64e8dc52e0df6d9289987`;
the loaded native binary hash is
`d1efee507a768446fc8d314062ad579d1badf589c52d3a51b01dd048b7c512ab`.
All nine protected configuration, calibration, service-override and identity
file hashes remained unchanged. Both installed shared seed assets match the
branch's recorded-calibration defaults; the validated per-unit pair continues
to take precedence through the existing override.

The first 300-frame stream overlapped the requested board captures and delivered
9.900 fps with three delivered capture skips, p95 age 202.0 ms, and zero camera,
requeue, trigger or processing failures. A separate 120-frame check with no
concurrent capture consumer delivered **9.9995 fps**, zero delivered sequence
gaps, p95 age **101.95 ms**, and the same zero failure counts. Valid depths were
positive finite float32 values, invalid values were NaN, and conversion matched
`fB/disparity`. This validates transport and numeric processing; it does not
override the failed board-alignment result.

The final Pi state uses the Actions minimal binary, normal installed camera
libraries and the unchanged recovered calibration, stereo and exposure
controls. Earlier test binaries, extracted camera libraries, preload experiments
and transient overrides were removed. The protected hashes were checked again.
Collected reports are in `output/calibration-alignment-test-evidence-20261021/`;
full raw captures remain in the named evidence folders on the Pi and workstation.
