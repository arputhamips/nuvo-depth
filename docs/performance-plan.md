# Performance and acceptance plan

## Primary target

- 728 x 544 disparity at a stable 6-10 Hz.
- Latest-frame-wins latency; no unbounded queue.
- Minimize total Pi CPU after correctness is established.
- Report both Linux process CPU (`100%` equals one fully occupied core) and
  normalized four-core SoC CPU so percentages are unambiguous.

No numeric CPU promise is accepted until it is measured on the supplied Pi,
camera rig, and Hailo-8L under a thermally stable 30-minute run.

The first isolated CPU SGBM replay baseline is recorded in
[cpu-sgbm-baseline.md](cpu-sgbm-baseline.md). It sustains 10 Hz with 9.039%
normalized four-core CPU and no deadline misses, excluding live capture and
rectification. It is a starting measurement, not final acceptance.

## Timing budget at 10 Hz

| Stage | Initial budget |
| --- | ---: |
| Pair acquisition and validation | 5 ms |
| Rectification/preprocess | 20 ms |
| Hailo inference | 55 ms |
| Crop/mask/result finalization | 5 ms |
| ROS/IPC handoff | 5 ms |
| Slack | 10 ms |

The stages are pipelined with fixed buffers, but only one newest pending pair is
retained. The real budgets will be replaced by measured p50/p95/p99 values.

## Optimization order

1. Measure capture-only CPU and drops.
2. Precompute fixed-point rectification maps and remove all per-frame allocation.
3. Keep RGB layout conversions inside libcamera/Hailo input handling where
   possible; never clone an image merely to cross an API boundary.
4. Use asynchronous HailoRT vstreams with a small fixed buffer pool.
5. Store final disparity as contiguous float32 once; the ROS bridge only wraps or
   copies the required message payload.
6. JPEG only the already-rectified left image at 2 Hz, using hardware encoding if
   the Pi path is stable, otherwise a measured low-rate software encoder.
7. Disable debug images and future point cloud work in production by default.

## Required telemetry

- input, valid-pair, inference, disparity, and JPEG rates;
- pair delta, capture age, end-to-end p50/p95/p99 latency;
- per-stage CPU time, process RSS, and buffer high-water marks;
- drop reason counters;
- Hailo utilization/errors and temperature;
- output subscriber count and ROS publish duration.

## Test matrix

- daylight, shade transition, dusk/low texture, wet grass, repetitive texture;
- stationary and moving mower;
- one camera missing, trigger missing, Hailo reset, ROS bridge restart;
- 30-minute thermal soak and 8-hour endurance run;
- recorded-pair replay with deterministic output hashes/tolerances.
