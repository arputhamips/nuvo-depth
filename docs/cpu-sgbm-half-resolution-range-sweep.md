# Half-resolution CPU SGBM disparity-range sweep

## Scope

This test keeps the selected spatial processing path and filtering constant:

- 728 x 544 rectified input;
- 364 x 272 SGBM working image;
- block size 5, uniqueness 1, speckle 30/2;
- `SGBM_3WAY`, one OpenCV thread;
- auto-exposed synchronized input.

Only the working disparity count and corresponding full-resolution output limit
change. Restoring from the half-resolution working image multiplies disparity
by two. It does not increase disparity precision or spatial detail.

## Recorded-pair replay

| Working / restored range | Near limit | Fixed left boundary | Compute p50 | Normalized CPU | Full valid | Searchable valid |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 80 / 160 | 0.498 m | 160 px | 34.672 ms | 7.684% | 63.9% | 82.0% |
| 96 / 192 | 0.415 m | 192 px | 36.486 ms | 8.495% | 60.2% | 81.8% |
| 128 / 256 | 0.311 m | 256 px | 39.361 ms | 9.089% | 53.5% | 82.6% |

Every replay sustained 10 Hz with zero deadline misses. Searchable-region
coverage remains effectively unchanged. The full-frame coverage reduction is
primarily the deterministic left search boundary growing with the range.

## Live 60-second result

| Metric | Current 80 / 160 | Candidate 128 / 256 | Change |
| --- | ---: | ---: | ---: |
| Output rate | 9.9999 Hz | 9.9999 Hz | none |
| Normalized process CPU | 9.84% | 11.07% | +1.23 points |
| Whole-SoC CPU | 9.87% | 11.64% | +1.77 points |
| SGBM p50 | 23.58 ms | 28.66 ms | +5.08 ms |
| Depth-ready p50 | 53.99 ms | 59.15 ms | +5.17 ms |
| Median full-frame valid | 65.25% | 54.22% | -11.03 points |
| JPEG output | 2.0 Hz | 2.0 Hz | none |
| Rejected/replaced ready pairs | 0 | 0 | none |
| Sensor delta p50 / max | 17 / 22 us | 18 / 25 us | negligible |

The 128-bin run held 8000 us / 2.0x exposure at median 69.5, and left/right
applied gain remained identical.

## Assessment

Increasing `numDisparities` does not add detail inside the existing range. SGBM
still operates on the same 364 x 272 pixels with the same 1/16-working-pixel
subpixel representation. It only searches closer objects and increases cost.

The current ROS obstacle projector rejects depth below 0.75 m. That boundary
requires approximately 106.25 full-resolution disparity pixels, or 53.1 at the
working resolution. The current 80 / 160 setting already covers it with ample
margin. Therefore, 128 / 256 adds no navigation range that the obstacle pipeline
will consume.

Keep 80 / 160 as the navigation default. Retain 128 / 256 only as an optional
near-field mode for a future consumer that needs depth between approximately
0.31 and 0.50 m and can accept the larger invalid boundary and CPU cost.

Artifacts are stored outside Git under
`artifacts/cpu_sgbm/half-d128-live-60s-20260712/` and on the Pi under
`/home/cam/cpu-sgbm-half-d128-live-60s-20260712/`.
