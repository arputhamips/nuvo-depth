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
