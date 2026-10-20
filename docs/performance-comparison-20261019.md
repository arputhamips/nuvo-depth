# Pi 5 mixed-stream performance comparison — 2026-10-19

The updated runtime reduces daemon CPU by **67.3%** in the requested mixed
mono 10 Hz/depth 1 Hz workload. With this unit's existing `hh` matcher, delivered
mono improves from about **3.26 to 7.03 fps** and depth stays at **1 fps**.
The full 10 Hz mono target remains unmet with these settings.

The [measurement evidence](performance-comparison-20261019.json) contains all
12 completed Pi cases, both Wi-Fi raw/API runs, desktop rendering results,
build/package hashes, and final state checks. Tested commits were
[`3b07816`](https://github.com/arputhamips/nuvo-depth/commit/3b078165d2fbf052a821afd050a18a67dbdf2aa9)
and [`c361700`](https://github.com/arputhamips/nuvo-depth/commit/c361700ca5ac30d008d0c753150069917c78c47d).
Both used successful ARM64 CI artifacts:
[before](https://github.com/arputhamips/nuvo-depth/actions/runs/39335890638),
[after](https://github.com/arputhamips/nuvo-depth/actions/runs/39342653499).
The intervening README-only commit does not change the runtime.

## Mixed workload, repeated

CPU is **percent of one core**: 100% means one fully used core, and this Pi has
four cores. Latency is capture to fully received/decoded SDK frame on the Pi,
including gateway and verified TLS. It excludes the physical Wi-Fi link and
browser. Each mixed measurement lasted 60 seconds after warm-up.

| Build / run | Daemon CPU | Gateway CPU | Mono fps | Depth fps | Mono latency p50 / p95 | Depth latency p50 / p95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Before 1 | 105.4% | 1.82% | 3.26 | 0.999 | 388 / 431 ms | 558 / 681 ms |
| Before 2 | 105.3% | 1.81% | 3.26 | 0.999 | 384 / 432 ms | 554 / 677 ms |
| After 1 | 34.4% | 1.97% | 7.03 | 0.999 | 31 / 330 ms | 380 / 394 ms |
| After 2 | 34.5% | 1.97% | 7.03 | 0.999 | 33 / 331 ms | 380 / 422 ms |

The updated matched-frame counter reports 1.000 Hz in both mixed runs, rather
than counting mono publications as depth work. The baseline has no reliable
matched-frame counter, so its matcher rate is not claimed.

The remaining long mono tail is material: its p95 is still about 330 ms.
Matching takes about 305 ms with this `hh` configuration, and image and matcher
requests still share one processing worker. Splitting demand cadence avoids
unnecessary matching, but does not make that worker concurrent. Independent
image processing is the next candidate if 10 Hz mono must hold with this matcher.
The repository's default `3way` profile was not measured in this record.

## Other Pi cases

| Requested output | Build | Daemon CPU | Gateway CPU | Delivered fps | Latency p50 / p95 |
| --- | --- | ---: | ---: | ---: | ---: |
| Idle | Before | 2.1% | 0.08% | — | — |
| Idle | After | 2.2% | 0.09% | — | — |
| Mono 10 Hz | Before | 3.1% | 2.06% | 9.92 | 77 / 122 ms |
| Mono 10 Hz | After | 3.0% | 2.11% | 9.93 | 72 / 121 ms |
| Depth + associated mono 1 Hz | Before | 33.6% | 0.72% | 1.00 | 373 / 380 ms |
| Depth + associated mono 1 Hz | After | 33.6% | 0.75% | 1.00 | 386 / 395 ms |
| Mono + depth 10 Hz | Before | 105.4% | 2.51% | 3.26 | 392 / 444 ms |
| Mono + depth 10 Hz | After | 105.2% | 2.55% | 3.28 | 395 / 438 ms |

Standalone mono and depth results are broadly unchanged. Both products at
10 Hz remain limited to roughly 3.3 fps by this matcher. Capture stayed close
to 10 Hz throughout; capture cadence is not delivered output cadence.
No decoded raw-stream errors or non-increasing sequences were observed, and
all recorded throttling flags were zero. These are bounded runs, not endurance
or metric-depth accuracy qualification.

## Workstation Wi-Fi and HTTP

A separate 30-second raw mixed run used the Windows SDK client over the Pi's
`wlan0` route, with the same matcher controls. Both builds used resolved IPv4
while retaining hostname and pinned-certificate verification. The host's
initial IPv6 connection attempt otherwise incurred a five-second fallback;
that abandoned attempt is excluded from the comparison.

| Build | Mono / depth fps | Health response p50 / p95 | TLS connections for 80 health requests |
| --- | ---: | ---: | ---: |
| Before | 3.26 / 0.98 | 43.8 / 96.3 ms | 80 |
| After | 7.00 / 1.00 | 5.3 / 32.2 ms | 3 |

The requests reused one HTTP client connection where supported, at about
five requests per second. The updated server's bounded keepalive materially
reduces reconnections. The advertised 24 MiB/s Wi-Fi budget is a resource limit,
not measured link capacity; this run does not qualify wired or USB throughput.
Cross-host capture latencies in the evidence are clock-aligned estimates;
the same-clock Pi measurements above are the primary latency result.

An additional encoded-stream comparison was incomplete: the updated build's
initial WebSocket upgrade returned HTTP 503 while a fresh source was not ready.
The test client did not retry that handshake. Encoded cold-start/retry behavior
and encoded throughput remain open qualification items; no before/after encoded
performance claim is made here.

## Browser responsiveness

A visible Chromium browser on Windows replayed synthetic 728 × 544 mono and
depth through each commit's actual `renderFrame`, frame codec, console DOM and
CSS. Warm-up preceded each run, in order before, after, after, before.

| Replay workload | Before render work | After render work | Reduction |
| --- | ---: | ---: | ---: |
| Mono 10 Hz / depth 1 Hz | 72.9 ms/s | 42.0 ms/s | 42.4% |
| Both at 10 Hz | 130.3 ms/s | 80.8 ms/s | 38.0% |

These values measure JavaScript spent inside the renderer, not browser process
CPU or complete paint time. Both versions sustained the replay rates and had
no measured animation-frame gaps over 50 ms. Mixed replay render-task p95
fell from 9.4–9.9 ms to 6.0–7.0 ms.

The user also confirmed successful long-token login and responsive Live view,
Overview and Outputs navigation during the live console check. This is a
qualitative confirmation, not a timed interaction benchmark. The earlier login
failures were caused first by the test's temporary console outage and then by
an expired short setup code in the initial test card. The existing device token
worked. No owner password was created for this performance test.

## Test controls and final state

- Pi 5 Model B Rev 1.1, 16 GiB, Debian 13 Trixie, kernel
  `6.12.75+rpt-rpi-2712`, `ondemand` governor, dual IMX296, 728 × 544 at 10 Hz.
- Matcher held fixed for measured comparisons: SGBM `hh`, search 96, block 5,
  two OpenCV threads, median 5, uniqueness 0, speckle 30/2. Exposure and
  calibration controls were not retuned.
- Mixed means independent authenticated subscriptions for `left_mono` at
  10 Hz and associated `depth,left_mono` at 1 Hz. FPS counts received frames
  over their arrival interval, not capture-sequence differences.
- Each Pi case used five seconds of warm-up; idle ran 15 seconds, standalone
  and combined cases 30 seconds, and mixed cases 60 seconds, twice per build.
  CPU came from service cgroup time deltas; the observer's CPU is separate.
- Runtime payloads were extracted from CI Debian packages. Temporary systemd
  overrides selected their binaries and web roots. Installed packages were
  unchanged. The original services and their commands are restored, all test
  overrides are removed, and no benchmark worker or restoration timer remains
  active.
- The original encoded mode is restored. The encoder's current `left_color`
  source selection from the manual browser check is preserved. Authentication
  failure/lockout audit updates from expired-code attempts are also preserved;
  the strict snapshot guard correctly detected those changes. Other protected
  configuration, identity/setup-private and boot files matched their original
  hashes. Ownership remains unclaimed and no owner password was added.

The replay does not qualify mobile browsers, background tabs, encoded video
decoding or slow clients. The temporary builds were tested without publishing
a new install release or replacing the Pi's installed packages.
