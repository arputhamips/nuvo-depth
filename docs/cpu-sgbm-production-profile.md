# CPU SGBM production profile

## Decision

The production CPU backend uses half-resolution OpenCV SGBM with a 128-pixel
restored disparity limit.  The selected profile is:

| Setting | Value |
|---|---:|
| Input and output | 728 x 544 |
| Matching resolution | 364 x 272 |
| Minimum disparity | 0 |
| Working disparity count | 64 |
| Restored disparity limit | 128 px |
| Block size | 5 |
| P1 / P2 | 200 / 800 |
| Mode | `MODE_SGBM_3WAY` |
| Uniqueness ratio | 1 |
| Speckle window / range | 30 / 2 |
| Left-right maximum difference | 1 |
| Prefilter cap | 31 |
| OpenCV threads | 1 |

At the calibrated `f*b = 79.6867 px m`, 128 pixels reaches approximately
0.623 m.  This includes the ROS obstacle pipeline's 0.75--6.0 m operating
band.  The working disparity is multiplied by two when the disparity image is
restored to 728 x 544; 64 working bins therefore means 128 full-resolution
pixels, not a 64-pixel final range.

## Sweep method

Six hardware-triggered, rectified stereo pairs were recorded at 728 x 544.
Left/right sensor timestamp deltas were 15--16 us and both cameras reported
the same exposure and gain.  Geometry and confidence settings were evaluated
on every pair.  Finalists were then run live for 300 measured frames at 10 Hz,
with 20 warm-up frames, shared auto exposure, 2 Hz color JPEG, and metric depth
disabled.

The geometry sweep covered all 16 requested combinations:

| Restored range | Working bins | Block sizes |
|---:|---:|---|
| 128 | 64 | 5, 7, 9, 11 |
| 150 | 80, masked at 150 | 5, 7, 9, 11 |
| 160 | 80 | 5, 7, 9, 11 |
| 192 | 96 | 5, 7, 9, 11 |

OpenCV requires disparity counts divisible by 16, so the requested 150-pixel
case used 80 working bins and an explicit 150-pixel restored-output mask.  It
was effectively identical to 160 in this scene.

Representative geometry results from the Pi replay are:

| Range / block | Compute p50 | Four-core-normalized CPU | Searchable valid |
|---|---:|---:|---:|
| 128 / 5 | 24.00 ms | 6.02% | 83.3% |
| 128 / 7 | 31.85 ms | 7.32% | 85.0% |
| 128 / 11 | 24.65 ms | 6.18% | 87.2% |
| 160 / 5 | 34.94 ms | 8.51% | 82.0% |
| 192 / 5 | 37.61 ms | 9.08% | 81.8% |

Block 5 retained the best photometric and native-resolution agreement.  Block
7 was temporally steadier and removed small islands, but smoothed more object
detail.  Blocks 9 and 11 progressively increased density by smoothing across
boundaries and were rejected for navigation.

## Matcher-setting sweep

For block 5 and block 7, the staged sweep covered:

- uniqueness ratios 0, 1, 3, 5, 8, 10, and 15;
- speckle windows 0, 10, 20, 30, 50, and 100;
- speckle ranges 1, 2, and 4;
- left-right thresholds disabled, 1, 2, and 4;
- prefilter caps 15, 31, and 63;
- SGBM, HH, 3WAY, and HH4 aggregation modes;
- P1 scales 0.5, 0.75, 1.0, 1.5, and 2.0;
- P2/P1 ratios 2, 4, 6, and 8.

The principal block-5 results were:

| Variant | Replay compute p50 | Searchable valid | Photo MAE | Native bad >2 px | Temporal std p50 |
|---|---:|---:|---:|---:|---:|
| Selected baseline | 21.30 ms | 84.52% | 7.250 | 25.11% | 0.216 px |
| Uniqueness 0 | 19.51 ms | 84.89% | 7.251 | 25.24% | 0.218 px |
| Speckle disabled | 20.43 ms | 89.23% | 7.255 | 26.95% | 0.261 px |
| LR threshold 2 | 21.26 ms | 85.12% | 7.257 | 25.24% | 0.219 px |
| P2/P1 ratio 6 | 21.23 ms | 88.50% | 7.410 | 28.41% | 0.247 px |
| Standard SGBM | 32.36 ms | 83.68% | 7.302 | 25.76% | 0.183 px |
| HH | 43.36 ms | 85.19% | 7.286 | 27.83% | 0.185 px |
| HH4 | 40.05 ms | 84.61% | 7.250 | 26.92% | 0.205 px |

Disabling or relaxing filters produced denser images, but those pixels had
worse temporal or native-resolution agreement.  Larger smoothness penalties
also filled holes while increasing boundary disagreement.  Higher uniqueness
ratios removed too much supported area.  The original confidence values are
therefore retained.

## Adjacent live validation

| Profile | Rate | SGBM p50 / p95 | Depth-ready age p50 | Process CPU, four-core normalized | Valid p50 |
|---|---:|---:|---:|---:|---:|
| Previous 160 / block 5 | 10.000 Hz | 24.108 / 30.383 ms | 54.639 ms | 9.967% | 47.15% |
| **Selected 128 / block 5** | **10.000 Hz** | **21.710 / 27.376 ms** | **52.276 ms** | **9.373%** | **51.89%** |
| 128 / block 5, LR 2 | 10.000 Hz | 21.699 / 27.332 ms | 52.290 ms | 9.407% | 53.18% |
| 128 / block 7 | 10.000 Hz | 21.898 / 27.582 ms | 52.509 ms | 9.472% | 53.86% |

Every live run had only one rejected pair during startup, sensor delta p95 no
worse than 20 us, and no measured-rate loss.  Relative to the previous profile,
the selected profile saves 2.40 ms of median matching time, 2.36 ms of median
depth-ready latency, and 0.59 percentage points of normalized process CPU.

## Evidence

- Recorded inputs and all geometry results:
  `artifacts/cpu_sgbm/production-sweep-20260712/pairs` and `geometry`
- All 58 staged matcher-setting outputs and CSV metrics:
  `artifacts/cpu_sgbm/production-sweep-20260712/settings-native`
- Four adjacent live reports and final frames:
  `artifacts/cpu_sgbm/production-sweep-20260712/live`
- Reproducible sweep tool: `tools/pi/sweep_sgbm_production.py`

This profile is the default for the CPU backend.  Larger ranges remain useful
as explicit diagnostic profiles for objects closer than 0.623 m, but they are
outside the current autonomy depth band and should not consume production CPU.
