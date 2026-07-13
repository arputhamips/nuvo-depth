# CPU SGBM invalid-pixel coverage research

## Scope

The original experiment used this production geometry and compute path:

- 728 x 544 rectified output;
- 364 x 272 grayscale SGBM input;
- 96 working disparities / 192 full-resolution disparity range;
- `SGBM_3WAY`, block size 5, one OpenCV thread;
- 10 Hz disparity and 2 Hz color JPEG;
- metric-depth generation disabled.

The goal is to reduce invalid interior pixels without converting ambiguous or
occluded regions into authoritative free-space depth.

## Why visible pixels can still be invalid

A feature appearing in both RGB images is not sufficient for a safe dense
match. Repetitive texture, weak gradients, reflections, depth discontinuities,
partial occlusion, and resampling can give several similar candidate matches.
OpenCV SGBM can reject a result through its uniqueness test, left/right
consistency threshold, and connected-component speckle filter.

The approximately 192-pixel black band on the left in the original baseline is
different: it is the fixed search-range boundary produced by 96 working
disparities. Filter tuning cannot recover it while preserving the complete
192-pixel range. A second low-disparity pass or a smaller primary range is
required.

## Recorded-pair sweep

Every variant replayed the same synchronized, rectified pair. Coverage is the
fraction of the complete 728 x 544 image; searchable coverage excludes the
fixed disparity-search boundary.

| Variant | Valid | Searchable valid | Result |
| --- | ---: | ---: | --- |
| Baseline: uniqueness 5, speckle 50/2 | 58.42% | 79.4% | high-confidence reference |
| Uniqueness 2 | 60.13% | 81.7% | changes some accepted disparities |
| Uniqueness 0 | 60.76% | 82.5% | admits more ambiguous matches |
| Speckle window 25 | 59.61% | 81.0% | adds 1.18 points; preserves all reference values |
| Speckle window 20 | 60.28% | 81.9% | adds 1.85 points; preserves all reference values |
| Speckle window 15 | 60.8% | 82.6% | adds about 2.4 points |
| Speckle window 10 | 61.41% | 83.4% | adds 2.99 points; preserves all reference values |
| Speckle disabled | 62.67% | 85.1% | adds 4.25 points, including many small islands |
| Uniqueness 2 plus speckle disabled | 64.61% | 87.8% | densest but highest ambiguity risk |

Changing block size to 7 or 9 replaced substantial parts of the disparity map
rather than simply recovering holes. Relaxing `disp12MaxDiff` to 2 or 4 added
only 0.29 or 0.82 percentage points, mostly as small components with worse
photometric residuals. Disabling the check produced no change in this scene.
Unmasked WLS filtering was already rejected because it filled occlusions;
confidence-masked WLS was slower and substantially sparser.

For the speckle-window-10 replay, no baseline-valid pixel was removed or
changed. The added pixels had a median left/right photometric residual of 3
intensity levels versus 3 for baseline pixels; p95 was 19 versus 15. This is a
useful proxy, not ground truth.

## Selected 80 / 160 candidate

OpenCV requires the working disparity count to be a multiple of 16. Therefore,
80 working disparities and 160 restored full-resolution disparities are the
closest legal step to the requested range of roughly 150 pixels. The candidate
also uses uniqueness 1 and speckle window/range 30/2.

On the same recorded pair, the candidate raised complete-frame valid coverage
from 58.42% to 65.27% and searchable coverage from 79.4% to 83.7%. It added
7.62% of the frame and removed 0.78%. Of pixels valid in both maps, 92.1% were
within 0.5 disparity pixels and 93.6% were within one disparity pixel. Mean
left/right photometric residual was effectively unchanged (4.52 baseline,
4.53 candidate), and p95 remained 15 intensity levels. The added pixels had a
median residual of 3 and p90 of 12.

The smaller range reduces the deterministic left invalid band by 32 pixels. It
also changes the calibrated near limit from approximately 0.42 m to 0.50 m.
Only 0.10% of this recorded frame contained baseline-valid disparity at or
above 160 pixels, but close-obstacle scenes still need explicit field testing.

## Live A/B

Two consecutive 600-frame camera runs compared window 50 with window 10. Both
used synchronized external triggering, zero-copy input leases,
grayscale-before-rectification, metric depth disabled, and the 2 Hz JPEG path.

| Metric | Window 50 | Window 10 |
| --- | ---: | ---: |
| Rate | 9.9999 Hz | 9.9999 Hz |
| Median valid fraction | 59.71% | 62.96% |
| Minimum valid fraction | 57.17% | 60.74% |
| Normalized process CPU | 10.33% | 10.29% |
| Whole-SoC CPU | 9.93% | 9.85% |
| Depth-ready p50 | 55.99 ms | 55.88 ms |
| Sensor delta p50 / max | 18 / 24 us | 18 / 22 us |
| Rejected or replaced pairs | 0 | 0 |

The candidate therefore adds roughly 3.25 percentage points of live coverage
without a measurable performance or synchronization penalty.

An immediately adjacent 600-frame live A/B then compared the selected 80/160,
uniqueness-1, speckle-30 candidate against the original 96/192,
uniqueness-5, speckle-50 control:

| Metric | Original control | Selected candidate | Change |
| --- | ---: | ---: | ---: |
| Rate | 9.9999 Hz | 9.9999 Hz | none |
| Median valid fraction | 59.77% | 67.27% | +7.50 points |
| Minimum valid fraction | 57.18% | 65.44% | +8.27 points |
| SGBM p50 | 25.73 ms | 23.54 ms | -2.18 ms |
| Depth-ready p50 | 55.96 ms | 53.74 ms | -2.22 ms |
| Normalized process CPU | 10.29% | 9.77% | -0.52 points |
| Whole-SoC CPU | 10.06% | 9.52% | -0.54 points |
| Rejected or replaced pairs | 0 | 0 | none |

## Recommendation

Use 80/160 disparities, uniqueness 1, and speckle window/range 30/2 as the CPU
backend default. It improves both coverage and compute cost in recorded and
live tests. Treat the approximately 0.50 m near limit and the lower uniqueness
threshold as explicit operating constraints. Before declaring the tuning
field-stable, check grass, gravel, repetitive texture, moving objects, changing
illumination, and obstacles closer than 0.50 m.

The next implementation should calculate speckle confidence tiers from one raw
SGBM result:

- components larger than 50 working pixels: high confidence;
- components from 11 through 50: tentative coverage;
- components of 10 or fewer: invalid;
- uniqueness and left/right-consistency failures: invalid.

This exposes the measured window-10 coverage without treating it as equivalent
to established depth. A future obstacle layer can conservatively consume only
high-confidence pixels, while diagnostics and visualization can show the
tentative tier. It also avoids a second SGBM pass.

The full reports and images remain outside Git under
`artifacts/cpu_sgbm/coverage-tests-20260712/` and
`artifacts/cpu_sgbm/coverage-live-20260712/`. The 80/160 recorded-pair A/B is
under `artifacts/cpu_sgbm/disparity-d80-u1-s30-20260712/`; its live reports are
also retained on the Pi under `/home/cam/cpu-sgbm-d80-u1-s30-60s` and
`/home/cam/cpu-sgbm-control-d96-u5-s50-60s-20260712`.

## References

- OpenCV `StereoSGBM` parameter documentation:
  <https://docs.opencv.org/master/d2/d85/classcv_1_1StereoSGBM.html>
- OpenCV WLS confidence-filter documentation:
  <https://docs.opencv.org/3.4/d9/d51/classcv_1_1ximgproc_1_1DisparityWLSFilter.html>
- Hirschmueller, *Stereo Processing by Semiglobal Matching and Mutual
  Information*:
  <https://elib.dlr.de/55367/>
