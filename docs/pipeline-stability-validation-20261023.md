# Pipeline stability qualification — 2026-10-23

Code `fe92b2140d0699bd503bfcb29679c0f85e924ae5` passed host regressions, an ARM64 artifact build and
the bounded live Pi checks below. Later documentation changes do not change this
tested executable. This is development qualification, not a tagged release.

The [ARM64 Actions artifact](https://github.com/arputhamips/nuvo-depth/actions/runs/40017191347) contains the source,
packages, dependency record and 67 passing CTests. [Hosted checks](https://github.com/arputhamips/nuvo-depth/actions/runs/40017182261)
passed native, SDK, browser and packaging checks. Local full/minimal CTest runs
passed 67/67 and 52/52; seven focused ASan/UBSan tests passed. Desktop/mobile browser
journeys used a real TLS gateway fixture. Regression coverage and implementation
entry points are in the [code map](native-runtime-ipc-gateway.md#source-map).

## Pi setup and workload results

Pi 5, 16 GiB RAM, Debian 13, kernel 6.12.75, libcamera 0.7.2+rpt20261009 and
OpenCV 4.10.0. Capture and matching were 728 × 544 at 10 Hz; SGBM 3-way searched
128 disparities with two OpenCV threads. Existing beta5 calibration geometry and
its provisional authority were preserved. The artifact binary was extracted to
a temporary service; installed packages and production settings were not replaced.

Each comparison measured 45 seconds after five seconds of warmup, using identical
copied manual controls and the same scene. RSS is resident memory in MiB. CPU is
relative to one core (100% equals one fully occupied core).

| Workload | RSS median, installed → candidate | CPU %, installed → candidate | Delivery |
| --- | ---: | ---: | --- |
| Rectified image, 10 Hz | 149.2 → 116.6 | 7.3 → 6.9 | Requested rates met |
| Depth, 10 Hz | 150.8 → 135.7 | 151.7 → 152.9 | Requested rates met |
| Image 10 Hz + depth 1 Hz | 150.8 → 136.9 | 17.8 → 18.2 | Requested rates met |
| Image + depth, 10 Hz each | 151.2 → 137.0 | 154.9 → 154.0 | Requested rates met |

All four comparisons reported zero new core errors, sensor sequence gaps, scheduler
storage/completion drops, swap or throttling. Memory improved; matcher CPU time
was essentially unchanged. A 10 Hz capture target does not measure maximum FPS.
Depth-only capture-to-reply p95 changed from 107.57 to 113.10 ms; mixed image/depth
p95 changed from 114.07 to 111.82 ms. These sequential samples do not establish a
latency improvement or regression. Stage percentiles use unequal sampling rates
(candidate about 3.33 Hz, baseline about 10 Hz) and are not per-frame distributions.

The five-minute mixed image/depth run delivered the requested rates with
0 new core errors. Resident memory peaked at
171.8 MiB after the earlier all-product and preview workloads;
temperature peaked at 68.3 °C, with zero swap and no
throttling. Retained allocations make this RSS different from the earlier
45-second comparison. The [machine-readable record](pipeline-stability-validation-20261023.json) retains rates,
latencies, sampled stage timings, copy counters and source-summary hashes.

## Exposure and output checks

- Default target 75 / band 65–85 passed a 90-second well-lit stationary test. Its final
  30 seconds held median 67, with zero exposure updates and matching reported
  shutter/gain on both cameras.
- The initial dark recording failed brightness stability: median 5, p05–p95 4–8.
  The command remained at its upper limit. Its initial manual/reload samples were
  a test-start artifact; the brightness failure remains a separate recorded limit.
- Turning on the available light moved the default configuration into its band.
  No adjustment was required, so that run's expected-decrease assertion failed.
  This is not evidence of a controller failure or a passed active-response test.
- To exercise both directions with available illumination, a separate temporary
  target 40 / band 35–45 test settled at 8 ms and gain 1.7×. Switching the light off
  increased gain to 3×; switching it on decreased gain to 1.85×. Both adjustment
  runs were monotonic. The final 30-second window passed the unchanged checks,
  held median 44–45, and had no further updates. Event times are
  human confirmations, not instrumented switching timestamps.
- All 14 raw products passed full SDK payload decoding at 1 Hz. JPEG left-color,
  JPEG rectified-color and H.264 left-color each delivered 200 measured frames in
  20 seconds. The first payload from each was independently decoded at 728 × 544,
  including a fresh FFmpeg H.264 decoder. Direct-I420 paths recorded zero RGB
  conversions; rectified-color recorded the expected conversion/remap work.

## Restoration and limits

The installed native daemon and gateway were restored. Protected production
configuration, device identity and calibration hashes remained unchanged.
No tags or releases were created. Camera images and raw traces remain local.

These checks do not qualify physical XTR edge timing, quantitative image noise or
detail, calibrated metric accuracy, smaller-memory boards, higher capture rates
or long-duration endurance. The dark-scene luma variation has not been attributed
to sensor noise, illumination flicker or ISP behavior. Further remap-grid changes
and fused disparity/depth passes remain measured optimization candidates.
