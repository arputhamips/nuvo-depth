# Live CPU pipeline profile

## Scope

This profile measures the temporary Python camera-to-depth validation path on
the supplied Pi 5. It uses the production-intent geometry and current tuned CPU
backend settings:

- two IMX296 cameras at 728 x 544 and 10 Hz;
- logical left camera 1 with `front_right.yaml` and logical right camera 0 with
  `front_left.yaml`;
- fixed-point OpenCV rectification maps;
- 364 x 272 SGBM input, 96 working disparities (192 full-resolution pixels),
  block size 5, and `SGBM_3WAY` with one OpenCV thread;
- full-resolution float32 disparity and metric depth buffers;
- a quality-75 rectified-left JPEG at 2 Hz.

The final run measured 30 pairs after six warm-up pairs. It does not include ROS
message construction, DDS serialization, or transport because that boundary is
not implemented yet. The output-buffer copies are the current handoff proxy.

## Result

The non-flushing capture path sustains 9.9996 Hz. The process used 49.77% when
one fully occupied core is 100%, or 12.44% normalized over the Pi's four cores.
Whole-SoC utilization was 12.29% during the same interval.

| Stage | p50 (ms) | p95 (ms) | Interpretation |
| --- | ---: | ---: | --- |
| Pair acquisition, wait and copy | 55.346 | 55.602 | Mostly sleeping until the next sensor frames |
| Left array copy | 0.682 | 0.745 | CPU work, concurrent with right capture |
| Right array copy | 0.701 | 0.733 | CPU work, concurrent with left capture |
| Rectify two full-color frames | 13.547 | 13.587 | Avoidable high-bandwidth CPU work |
| Convert two frames to grayscale | 0.895 | 0.899 | CPU work |
| Downscale two grayscale frames | 0.125 | 0.127 | CPU work |
| SGBM matching | 25.732 | 25.787 | Largest active compute stage |
| Restore and mask full disparity | 2.668 | 2.785 | Required disparity finalization |
| Derive metric depth | 0.881 | 0.998 | Optional for the currently required outputs |
| JPEG encode | 2.439 | 7.577 | Runs at 2 Hz; first encode was 9.286 ms |
| Copy disparity and depth outputs | 0.623 | 0.846 | Can be reduced with one required output/fixed buffers |
| Complete loop | 100.060 | 102.575 | Stable 10 Hz after warm-up |

The normal non-JPEG compute path is about 44.47 ms per pair. SGBM accounts for
about 58% of that active path and color rectification about 30%. JPEG costs only
about 0.49 ms per depth frame when amortized at 2 Hz.

`capture_request(flush=True)` must not be used in the serial production loop.
It requires exposure to begin after each capture call, so the 100 ms sensor
period is added after processing. The measured result was 150.05 ms p50 per
cycle, or 6.67 Hz. With flushing disabled after warm-up, frames were about
19.3 ms old when returned and the loop sustained 10 Hz. The production C++ path
should use asynchronous completion callbacks and a bounded latest-pair slot,
while rejecting duplicate or stale sequence/timestamp values explicitly.

## Synchronization finding

Both sensors have a stable 100.000 ms period, but `SyncReady` was false for every
measured pair and their median timestamp delta was 43.591 ms. At pair-ready the
left frame was 63.06 ms old and the right frame was 19.46 ms old. At depth-ready
they were 107.64 ms and 64.05 ms old respectively.

This is not a CPU bottleneck, but it is the largest current correctness issue.
Timing improvements cannot recover scene consistency from frames captured
43.6 ms apart.

## Measured optimization experiments

The same raw stereo pair was replayed to compare preprocessing layouts.

| Layout | p50 (ms) | Valid pixels | Median / p95 disparity change |
| --- | ---: | ---: | ---: |
| Full color rectify, then gray and resize | 14.698 | 54.21% | reference |
| Gray, full gray rectify, then resize | 6.409 | 54.02% | 0 / 0.313 working px |
| Gray, rectify directly to 364 x 272 | 2.428 | 52.75% | 0.125 / 2.125 working px |

The safe first change is grayscale-before-rectification. The rectified full-color
left frame should be produced only on the 2 Hz compressed-image schedule. That
color output costs 6.795 ms to rectify and 2.434 ms to encode. Amortized across
the 10 Hz depth stream, this layout should save about 6.8 ms of CPU time per
pair and reduce normalized process CPU from roughly 12.44% to about 10.7%.
That estimate must be replaced with a live measurement after integration.

Direct working-resolution rectification saves another 3.98 ms over the safe
layout, but it changes the effective resampling filter and produced a 2.125-pixel
p95 difference at working resolution (4.25 pixels at final resolution). Keep it
behind an experimental option until scene and range-error acceptance tests pass.

Changing the OpenCV SGBM execution mode does not help:

| Mode | p50 (ms) |
| --- | ---: |
| SGBM_3WAY | 25.526 |
| SGBM | 40.176 |
| HH4 | 48.849 |
| HH | 53.945 |

## Recommended implementation order

1. Build capture around asynchronous camera completions, a fixed buffer pool,
   explicit timestamp/sequence validation, and a one-entry latest-pair handoff.
   Never flush once per processed frame.
2. Fix hardware/libcamera synchronization and reject pairs outside the chosen
   tolerance before judging depth quality.
3. Convert to grayscale first, rectify the two grayscale depth inputs, and
   rectify the color left image only when its 2 Hz JPEG output is due.
4. Keep `SGBM_3WAY` and one OpenCV thread. Preallocate every OpenCV destination
   in C++ so the steady loop performs no heap allocation.
5. Publish the required float32 disparity directly. Make metric-depth generation
   optional; omitting its array plus the second output copy saves about 1.2 ms
   per frame in this prototype.
6. Hold camera requests until processing no longer needs their memory instead of
   copying both input arrays. The two current copies cost about 1.38 ms of total
   CPU time per pair.
7. Measure ROS message wrapping, serialization, and DDS publish time once the
   boundary exists. Do not optimize JPEG hardware acceleration yet: steady
   software JPEG is only 2.44 ms at 2 Hz.

The raw reports and captured pair are under the ignored
`artifacts/cpu_sgbm/pipeline-profile/` directory. The runs are reproducible with
`tools/pi/profile_cpu_pipeline.py` and
`tools/pi/benchmark_preprocess_variants.py`.
