# CPU SGBM resolution and disparity test matrix

Measured on 2026-07-12 on the supplied Raspberry Pi 5. Every run replays the
same focused, correctly rectified 728 x 544 pair. Capture and rectification CPU
are excluded. The requested `728 x 566` test was treated as `728 x 544`, because
the calibrated source is 544 pixels tall and synthesizing 22 rows would not add
stereo information.

OpenCV requires `numDisparities` to be a multiple of 16. An exact 196-pixel
limit is therefore implemented by searching the next legal number of bins and
invalidating output disparities at or above 196. At half resolution this means
112 working bins (224 full-resolution pixels of search coverage); at full
resolution it means 208 bins.

## One-thread results

| Test | Working geometry | Search / output limit | Quality settings | Rate | Compute p50 / p99 | Four-core CPU | Searchable valid | Misses at 10 Hz |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| Tuned baseline | 364 x 272 | 96 / 192 | block 5, uniqueness 5, speckle 50/2 | 10.000 Hz | 37.326 / 39.180 ms | 9.100% | 73.0% | 0 |
| Test 1 | 364 x 272 | 112 / 196 | block 5, uniqueness 15, speckle 100/1 | 10.000 Hz | 37.152 / 39.516 ms | 8.606% | 58.3% | 0 |
| Test 2 | 728 x 544 | 192 / 192 | block 5, uniqueness 10, speckle 100/2 | 5.533 Hz | 180.530 / 181.866 ms | 24.992% | 50.6% | 100/100 |
| Test 3 | 728 x 544 | 208 / 196 | block 5, uniqueness 15, speckle 100/1 | 5.360 Hz | 186.457 / 186.928 ms | 24.996% | 43.8% | 100/100 |

The strict filters reduce accepted density rather than improving throughput or
changing the scene geometry. Full-resolution matching retains finer spatial
edges, but it exposes more high-frequency speckle and cannot meet the requested
6-10 Hz rate on one core.

A later auto-exposed native-resolution test with a smaller accepted interval
does reach the 6 Hz lower bound. See
[cpu-sgbm-native-min8-max128.md](cpu-sgbm-native-min8-max128.md) for its detail,
latency, CPU, and frame-replacement tradeoffs.

The current auto-exposed 80, 96, and 128-bin half-resolution comparison is in
[cpu-sgbm-half-resolution-range-sweep.md](cpu-sgbm-half-resolution-range-sweep.md).

## Depth consistency

| Region | Test 1 | Test 2 | Test 3 |
| --- | ---: | ---: | ---: |
| Foreground box median | 0.654 m | 0.663 m | 0.664 m |
| Ladder region median | 1.458 m | 1.476 m | 1.472 m |
| Garage wall median | 5.763 m | 5.642 m | 5.642 m |

The stable medians show that all three configurations recover the same basic
geometry. Their differences are density, edge detail, noise, and CPU cost.

## Four-thread supplement

Increasing OpenCV from one to four threads made both full-resolution tests
slower. Test 2 fell to 3.890 Hz while consuming 95.607% of the four-core SoC;
test 3 fell to 3.659 Hz at 96.287%. `MODE_SGBM_3WAY` already has internal path
parallelism, and additional OpenCV workers create overhead/oversubscription on
this Pi. One worker remains the correct setting.

## Decision

Keep the tuned 364 x 272 / 96-bin baseline for the initial 6-10 Hz daemon. It
has the highest accepted density, stable depth, and the lowest practical CPU
cost. Test 1 is useful as a conservative mode, but it discards too much valid
depth for no measurable speed benefit. Tests 2 and 3 are quality-reference
modes rather than viable real-time defaults.
