# Independent image/depth processing — Pi retest, 2026-10-20

Independent workers sustain approximately **10 Hz mono alongside 1 Hz depth**
with the existing `hh` controls. The earlier single-worker build delivered
7.03 fps mono. Mono p95 latency falls from
329 ms to approximately 120 ms;
daemon CPU remains around 35% of one core.
Encoded cold starts now succeed without client retries.

[Sanitized measurement evidence](performance-independent-processing-20261020.json)
contains the results, exact package hashes, build records and restoration checks.
The previous runtime is `c361700`; the final tested runtime is
[`c5eef0b`](https://github.com/arputhamips/nuvo-depth/commit/c5eef0b68b65d0d66d6334ad42fc4c2915a628f1),
from [ARM64 CI run 39465250004](https://github.com/arputhamips/nuvo-depth/actions/runs/39465250004).
This supersedes the mixed-stream limitation and open encoded-startup item in the
[earlier comparison](performance-comparison-20261019.md).

## Same matcher, repeated mixed measurements

CPU is percent of **one core**, on a four-core Raspberry Pi 5. Latency is sensor
capture to fully received/decoded SDK frame using verified TLS on the Pi's LAN
address. It includes native processing and the gateway; it excludes physical
Wi-Fi transfer and browser rendering. Each mixed run lasts 60 seconds after
five seconds of warm-up.

| Build / run | Daemon CPU | Gateway CPU | Mono fps | Depth fps | Mono latency p50 / p95 | Depth latency p50 / p95 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Previous | 34.4% | 1.97% | 7.03 | 0.999 | 31 / 329 ms | 400 / 437 ms |
| Independent workers 1 | 34.8% | 2.52% | 9.94 | 0.999 | 73 / 121 ms | 382 / 396 ms |
| Independent workers 2 | 34.8% | 2.55% | 9.94 | 0.999 | 79 / 120 ms | 394 / 427 ms |

The mono-only control delivered 9.93 fps, at
3.2% daemon CPU.
Mixed mono now reaches that standalone rate. Its median arrival latency can be
higher than the old mixed median; the improvement is steady delivery and removal
of the long matcher-induced tail. The matcher itself still takes approximately
300 ms with these controls. No 10 Hz depth claim is made.

All 130 capture identities common to the independent image and depth streams
in the final mixed runs had identical mono payload hashes, capture timestamps
and configuration epochs. No decoded stream errors, non-increasing sequences,
or image/depth association mismatches occurred. All throttling flags were zero.

## Encoded cold start

The old build reproduced HTTP 503 with both color and disparity, while native
encoding continued successfully with zero encoder failures. The gateway's
validator accepted only compact 32-digit UUIDs; the real encoder writes canonical
hyphenated UUIDs. Real packets were rejected before they could satisfy the first-frame
wait. The fix accepts both valid spellings while preserving bounds, scalar-type,
nonzero identity, clock, freshness and keyframe validation.

The final build starts each stream from no encoded consumer after a raw-to-encoded
mode change. Tests use one authenticated TLS WebSocket attempt per trial, with no
retry and no increase to the existing three-second readiness timeout.

| Source | Configured cadence | Successful starts | Upgrade plus first frame |
| --- | ---: | ---: | ---: |
| H.264 `left_color` | 10 Hz | 3/3 | 126–202 ms |
| H.264 `disparity` | 1 Hz | 3/3 | 460–530 ms |
| H.264 `disparity` | 10 Hz requested | 3/3 | 436–490 ms |

Every first H.264 packet contained SPS, PPS and an IDR frame (NAL types 7, 8 and
5), so it carries the headers and picture required for independent decoding.
The 10 Hz disparity request still runs at the throughput allowed by `hh`; this
cold-start check does not establish 10 Hz matching. These are protocol and packet
checks, not a new interactive browser measurement.
The earlier user-confirmed browser responsiveness result remains in the previous
report.

## Windows Wi-Fi and API responses

The first two cases targeted at least 30 measured seconds with 80 health
requests through one reusable HTTP client. Slow requests extended the raw case
to 33 seconds. The third stream in the second case is a 10 Hz H.264 color
preview, alongside 10 Hz raw mono and 1 Hz associated depth/mono.

| Case | Mono / depth / encoded fps | Health p50 / p95 | TLS connections for 80 requests |
| --- | ---: | ---: | ---: |
| mixed_raw | 6.97 / 0.99 | 202.8 / 352.8 ms | 3 |
| mixed_raw_and_encoded | 5.60 / 0.99 / 9.93 | 122.5 / 376.1 ms | 3 |

Full raw mono plus associated depth needs approximately **47.5 Mbit/s of
payload before TLS**. The Pi-local 10/1 Hz result does not guarantee that rate
through the current Wi-Fi/Windows SDK path. Both the old and new builds showed
receive-window backpressure and queued TCP bytes in the Wi-Fi rechecks.
The earlier report's Wi-Fi timings are not a fixed capacity measurement.

Follow-up cases targeted at least 20 seconds and used 50 health requests each.
Slow API requests extended the receive-buffer experiment to about 65 seconds.
The browser-style case uses only raw depth/associated mono at 1 Hz plus encoded color preview at 10 Hz.
The receive-buffer experiment changes only the workstation's socket buffer;
it does not retune the matcher or resize products. A 4 MiB receive buffer
increased queueing and API latency without meeting the full raw target, so this
change was not applied to the SDK or product.

| Follow-up case | Duration | Delivered fps in stream order | Health p50 / p95 |
| --- | ---: | ---: | ---: |
| final_mono_only | 20.0 s | 9.89 | 1.9 / 104.8 ms |
| final_browser_preview_and_depth | 20.0 s | 1.00 / 9.94 | 7.6 / 47.1 ms |
| baseline_mixed_raw_recheck | 20.0 s | 4.84 / 0.89 | 72.4 / 477.8 ms |
| final_mixed_raw_receive_buffer_4MiB | 64.9 s | 8.19 / 0.95 | 983.0 / 2543.1 ms |

The resolved IPv4 route retains hostname and pinned-certificate validation.
Cross-host capture latency values in the evidence are estimates, with clock
calibration uncertainty bounded by 0.45 ms.
The Pi-local latency measurements above use one CLOCK_BOOTTIME clock directly.

## Processing and subscription contract

The image and matcher lanes own separate stateful pipelines and bounded input
and completion queues. They share the original eight storage slots / 256 MiB
payload allowance as four slots / 128 MiB per lane. Explicit capture-lease sharing
returns each camera request exactly once, after both lanes release their mappings.
A depth result retains its own associated images. A late matcher completion cannot
roll the image cache back, and an image completion cannot consume a numeric
encoder plan. A shared bounded effective-revision history keeps configuration
epochs consistent across lanes, including skipped updates and formatting-only
control writes.

The measured raw workload uses two concurrent subscriptions:

- `products=left_mono&cadence_hz=10&delivery=best_effort`
- `products=depth,left_mono&cadence_hz=1&delivery=best_effort`

A single combined raw subscription still describes a coherent product set at one
cadence. Its associated images do not silently acquire a different capture identity.
The browser can also use a fast image preview alongside slower raw depth output.

## Validation and final state

- Pi 5 Model B Rev 1.1, 16 GiB, Debian 13 Trixie, kernel
  `6.12.75+rpt-rpi-2712`, `ondemand` governor, dual IMX296, 728 × 544 capture at 10 Hz.
- SGBM `hh`, search/max 96, min 0, block 5, two OpenCV threads, median 5,
  uniqueness 0, speckle 30/2, prefilter 31. Exposure and calibration are unchanged.
- Host CTest passed 57/57 before the effective-revision follow-up. ASan/UBSan
  passed the runtime, capture-lease, capture-runner and local-frame-server tests.
  The final ARM64 revision passed 58/58 tests and package-install smoke checks.
- CI package payloads were extracted for temporary runtime overrides; installed
  packages were not replaced. Original service commands, encoding configuration
  (including `left_color`), and all protected settings and credentials are restored.
  No test override, benchmark service or restoration timer remains active.
- No new install release was published. The Pi is back on its original installed
  build. Measurements are bounded performance checks, not endurance or metric
  depth accuracy qualification.
