# Full-resolution OpenCV StereoBM test

## Configuration

The same final auto-exposed 728 x 544 rectified pair was replayed through
OpenCV `StereoBM` and `StereoSGBM` with matched geometry and filtering:

- full-resolution 728 x 544 matching;
- minimum disparity 0, 160 disparity bins / 160-pixel output limit;
- block size 5 x 5;
- uniqueness 1;
- speckle window/range 30/2;
- left/right maximum difference 1;
- one OpenCV thread.

StereoBM's additional texture threshold was set to zero so it did not have an
extra rejection rule absent from the requested SGBM configuration.

## Results

| Metric | StereoBM | Full-resolution SGBM | Current half-resolution SGBM replay |
| --- | ---: | ---: | ---: |
| Achieved rate | 10.000 Hz | 6.029 Hz | 10.000 Hz |
| Compute p50 | 74.82 ms | 165.77 ms | 34.67 ms |
| Match p50 | 72.58 ms | 163.08 ms | 31.19 ms |
| One-core process CPU | 74.84% | 99.99% | 30.74% |
| Normalized four-core CPU | 18.71% | 25.00% | 7.68% |
| Full-frame valid | 7.68% | 54.88% | 63.9% |
| Searchable-region valid | 9.85% | 70.3% | 82.0% |
| 10 Hz deadline misses | 0 / 100 | 100 / 100 | 0 / 100 |

StereoBM's accepted pixels have good correspondence quality: mean photometric
residual is 5.82 intensity levels and p95 is 17. Among pixels accepted by both
algorithms, BM and SGBM differ by only 0.125 disparity pixels at p50 and
0.5 pixels at p95. However, that overlap is only 7.63% of the frame. BM-only
coverage is 0.06%, while SGBM-only coverage is 47.25%.

## Decision

Do not add this exact StereoBM configuration as a live backend. It is fast
enough for 10 Hz but rejects roughly 90% of the searchable image, so it cannot
support the dense compact obstacle cloud. It behaves as a sparse,
high-confidence subset of the SGBM result.

StereoBM normally relies on a larger block for density. Testing block sizes 9,
15, or 21 could recover coverage, but that would smooth boundaries and work
against the original goal of improving detail. The current half-resolution
SGBM remains both faster and much denser.

The ignored artifacts are under
`artifacts/cpu_sgbm/stereo-bm-full-d160-b5-20260712/` and on the Pi under
`/home/cam/stereo-bm-full-d160-b5-20260712/`.
