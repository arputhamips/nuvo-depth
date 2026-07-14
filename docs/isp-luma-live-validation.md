# PiSP luma capture validation

## Decision

The production capture profile is `isp_luma_rgb_left`:

```text
left request:  YUV420 main + RGB888 lores
right request: YUV420 main
depth input:   full-resolution Y plane from each YUV420 buffer
color input:   left RGB888 lores view, only when scheduled
color space:   sYCC
```

Both left outputs belong to the same libcamera request and therefore share the
same sensor timestamp, exposure, gain, transform, and crop. The mapped arrays
remain camera-owned zero-copy views. The visible 728-pixel region is sliced
from the PiSP-aligned stride without changing the calibrated image geometry.

`rgb_gray` remains a selectable rollback profile. It keeps the earlier RGB888
capture and OpenCV BGR-to-gray conversion without changing SGBM or output
contracts.

## Identical-request image equivalence

An isolated A/B harness enabled YUV420 and RGB888 on the same camera requests,
then compared PiSP Y against OpenCV grayscale before running the frozen matcher.

| Measurement | Result |
| --- | ---: |
| Raw/rectified luma mean absolute error | 0.151-0.162 intensity levels |
| Pixels within one intensity level | >99.99% |
| Disparity valid-mask agreement | 93.9-94.0% |
| Mean disparity difference | 1.07 full-resolution pixels |
| Disparity p95 difference | 5.33 full-resolution pixels |
| Compact-cloud mean points, ISP / RGB | 408.9 / 410.3 |
| Compact-cloud accepted samples, ISP / RGB | 10707.1 / 10707.7 |

Running two independently created SGBM matchers on the same input produced
identical output, confirming that the small downstream differences come from
the luma quantization at ambiguous texture and occlusion boundaries rather than
matcher nondeterminism. A last-frame 5 cm cloud comparison had 0.796 voxel
Jaccard agreement and approximately 3 mm median nearest-point separation.

## Matched 600-pair live soaks

Both profiles used the same cameras, external trigger, AE limits, calibration,
frozen 364 x 272 / 64-disparity SGBM settings, compact-cloud configuration, and
2 Hz color schedule.

| Metric | `rgb_gray` | `isp_luma_rgb_left` |
| --- | ---: | ---: |
| Measured pairs | 600 | 600 |
| Output rate | 9.99996 Hz | 9.99995 Hz |
| Process CPU, normalized over four cores | 9.8918% | 9.6257% |
| Whole-SoC CPU | 9.9733% | 9.6755% |
| Gray conversion p50 | 0.8943 ms | 0.0009 ms |
| Paired gray rectification p50 / p95 | 5.3615 / 6.6384 ms | 5.4017 / 6.6937 ms |
| SGBM p50 | 21.3302 ms | 21.3431 ms |
| Depth-ready age p50 / p95 | 54.1966 / 63.6979 ms | 53.2933 / 62.7613 ms |
| Ready-pair replacements | 0 | 0 |
| Timestamp rejects | 1 | 1 |
| Thermal throttling | none | none |

Direct PiSP luma removed approximately 0.893 ms of recurring preprocessing,
reduced normalized daemon CPU by 0.266 percentage points, and preserved the
10 Hz cadence, AE behavior, bounded capture lifecycle, disparity density, and
compact-cloud scale. The single timestamp rejection was the same startup
behavior in both runs and did not reduce measured output cadence.

Full JSON, logs, disparity arrays, and terminal images are preserved outside
Git under `artifacts/isp-luma-soak-20260714T203750Z/`.

## Rectification backend boundary

This capture change does not select GPU rectification. `cpu_opencv` remains the
default and fallback. The YUV/DMABUF profile creates the required zero-copy
input boundary for a separately gated `egl_gl_dmabuf` backend, which must pass
live pixel, disparity, compact-cloud, latency, FD-cache, and restart acceptance
tests before it can be enabled.
