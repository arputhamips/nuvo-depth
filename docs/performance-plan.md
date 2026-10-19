# Performance and acceptance plan

The measurements below include the earlier Python depth path. Use the
[native runtime reference](native-runtime-ipc-gateway.md) and
[release qualification record](release-qualification-20261019.md) for the
current appliance architecture and tested installation cohort.

## Primary target

- 728 x 544 disparity at a stable 6-10 Hz.
- Latest-frame-wins latency; no unbounded queue.
- Minimize total Pi CPU after correctness is established.
- Report both Linux process CPU (`100%` equals one fully occupied core) and
  normalized four-core SoC CPU so percentages are unambiguous.

No numeric CPU promise is accepted until it is measured on the supplied Pi,
camera rig under a thermally stable 30-minute run.

The first isolated CPU SGBM replay baseline is recorded in
[cpu-sgbm-baseline.md](cpu-sgbm-baseline.md). It sustains 10 Hz with 9.039%
normalized four-core CPU and no deadline misses, excluding live capture and
rectification. It is a starting measurement, not final acceptance.

The subsequent [live CPU pipeline profile](cpu-live-pipeline-profile.md)
measures capture through output-buffer readiness at 10 Hz, including
rectification, metric depth, and throttled JPEG work.

The [async zero-copy trigger result](async-zero-copy-trigger-results.md)
supersedes its free-running/copying capture measurements: hardware exposure is
now synchronized to tens of microseconds and camera input buffers are leased
rather than copied.

The [60-second CPU soak](cpu-sgbm-60s-soak.md) then verifies
grayscale-before-rectification at 10 Hz. Full-color left rectification runs only
for the 2 Hz JPEG output, reducing normalized process CPU to 10.49%.

The [production-profile sweep](cpu-sgbm-production-profile.md) evaluates 16
range/block geometries and 58 staged confidence and aggregation profiles. The
selected 64/128-disparity, block-5, uniqueness-1, speckle-30/2 configuration
covers the 0.75--6.0 m navigation band. An adjacent live comparison against
the previous 80/160 profile reduced normalized process CPU from 9.97% to
9.37% and median depth-ready age from 54.64 ms to 52.28 ms at 10 Hz.

The [shared stereo auto-exposure result](stereo-auto-exposure.md) replaces the
fixed 3 ms / 1.5x camera setting. One controller meters both images, drives the
common external-trigger shutter, and applies matched gain. Hard limits are
8 ms (1/125 s) and 3.0x. A 60-second run sustained 10 Hz with 9.84% normalized
process CPU and zero left/right applied-gain difference.

## Timing budget at 10 Hz

| Stage | Initial budget |
| --- | ---: |
| Pair acquisition and validation | 5 ms |
| Rectification/preprocess | 20 ms |
| CPU stereo matching | 55 ms |
| Crop/mask/result finalization | 5 ms |
| IPC handoff | 5 ms |
| Slack | 10 ms |

The stages are pipelined with fixed buffers, but only one newest pending pair is
retained. The real budgets will be replaced by measured p50/p95/p99 values.

## Optimization order

1. Measure capture-only CPU and drops.
2. Precompute fixed-point rectification maps and remove all per-frame allocation.
3. Use the captured luma planes for matching; keep color conversions on the
   lower-rate preview path and avoid copies at API boundaries.
4. Keep CPU stereo matching bounded by the qualified working geometry and
   disparity range; measure changes against the production profile.
5. Store final disparity as contiguous float32 once and reuse the published
   frame for admitted clients.
6. Encode preview only at the admitted cadence while a consumer needs it.
   The current Pi 5 path uses software FFmpeg encoding.
7. Disable debug images and future point cloud work in production by default.

## Required telemetry

- input, valid-pair, stereo-matching, disparity, and JPEG rates;
- pair delta, capture age, end-to-end p50/p95/p99 latency;
- per-stage CPU time, process RSS, and buffer high-water marks;
- drop reason counters;
- Pi utilization/errors and temperature;
- output subscriber count and IPC/gateway delivery duration.

## Test matrix

- daylight, shade transition, dusk/low texture, wet grass, repetitive texture;
- stationary and moving mower;
- one camera missing, trigger missing, depth service restart, client restart;
- 30-minute thermal soak and 8-hour endurance run;
- recorded-pair replay with deterministic output hashes/tolerances.
