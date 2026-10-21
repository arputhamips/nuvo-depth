# Cleaned runtime: preview and depth requested at 10 Hz — 2026-10-21

The cleaned runtime sustained about **9.94 fresh preview packets/s** while the
unchanged matcher completed about **3.2 depth frames/s**. Relative to preview
alone, preview throughput changed by −0.082% and −0.105% in two mixed runs;
preview interval P95 increased by 2.249 ms and 1.114 ms. These short runs show
no material preview throughput loss under this workload, rather than proving
that every latency or resource cost is identical.

This tests frozen source `098910edaa7291f450c65e8711fe9fecba941acd`, built by
[manual ARM64 workflow 39665155172](https://github.com/arputhamips/nuvo-depth/actions/runs/39665155172).
It precedes the later browser-login removal and installer changes for beta 5.
The artifact still carries `0.1.1~beta.4` package metadata; the source commit and
package hashes identify this test, not that published release. **This is not
qualification of the upcoming published beta 5 package cohort.**

Full sanitized measurements, package/executable hashes, settings, counters,
timing distributions and restoration checks are in the
[JSON evidence](performance-cleanup-pi-20261021.json).

## Workload and measurement

The Pi was a Raspberry Pi 5 Model B Rev 1.1 with 16 GB RAM, kernel
`6.12.75+rpt-rpi-2712` and the `ondemand` governor. Each case had a five-second
warm-up followed by a 60-second measured interval. Requests were:

- H264 `left_color` preview at 10 Hz through `/api/v1/encoded/stream`.
- Separate raw `depth,left_mono` at 10 Hz with `delivery=best_effort`.
- For the supplemental case, separate raw `left_mono` at 10 Hz alongside depth.

Both outputs used the existing 728 × 544 profile. Stereo remained SGBM `hh`,
96-pixel search range, disparity bounds 0–96, block size 5, median filter 5 and
two OpenCV threads. Calibration, exposure, trigger and other controls were
preserved. The temporary preview source was `left_color`; H264 retained the
existing 8 Mbps target, two encoder threads, keyframe interval 1, `veryfast`
preset and low-latency setting. No quality settings were weakened to increase
the depth rate.

Streams were consumed locally on the Pi over certificate-verified TLS.
FPS counts unique, strictly increasing device/run/epoch/capture identities,
using `(frames−1)/(last arrival−first arrival)`. The JSON also gives
`frames/measurement duration`. Raw and encoded UUID spellings were normalized
before comparison. Capture timestamps had to increase and remain fresh.

Packet latency is capture CLOCK_BOOTTIME to complete receipt and SDK envelope
validation. It excludes H264 decoding, browser rendering/display and LAN/Wi-Fi
delivery. CPU is cgroup CPU time divided by wall time; 100% means one core.
Client/observer CPU is recorded separately.

## Results

| Case | Preview FPS | Depth FPS | Native CPU, one-core % | Gateway CPU, one-core % | Maximum °C |
|---|---:|---:|---:|---:|---:|
| Preview only | 9.94906 | — | 17.31 | 0.81 | 42.45 |
| Depth only, requested 10 Hz | — | 3.26074 | 105.11 | 2.56 | 49.60 |
| Preview + depth, repeat 1 | 9.94089 | 3.21958 | 119.26 | 3.48 | 53.45 |
| Preview + depth, repeat 2 | 9.93859 | 3.20526 | 119.67 | 3.55 | 56.20 |
| Raw mono + depth | 9.92878 mono | 3.23509 | 105.98 | 4.66 | 58.40 |

| Encoded preview case | Fresh packets | Interval p50 / p95 / max, ms | Packet latency p50 / p95, ms |
|---|---:|---:|---:|
| Preview only | 597 | 100.498 / 101.192 / 146.979 | 79.729 / 124.450 |
| Mixed repeat 1 | 596 | 100.535 / 103.442 / 146.333 | 86.792 / 129.733 |
| Mixed repeat 2 | 596 | 100.538 / 102.306 / 147.277 | 88.825 / 128.134 |

Mixed preview packet-latency P95 increased by 5.283 ms and 3.684 ms. Depth
throughput was 1.26% and 1.70% below depth alone. These measured costs remain
visible in the evidence; the result does not claim zero contention.

Depth packet latency P95 was 444.389 ms alone and 448.992 / 447.832 ms mixed.
Capture counters advanced by 600 pairs in every measured case. Mixed encoded
production counters advanced by 600, while matcher publication counters advanced
by 193. Separate status and arrival boundaries can differ by one completion;
they are reported independently rather than substituting total publications
for depth output.

## Matcher capacity and associated products

| Case | Unique matcher samples / matched counter delta | Processing p50 / p95, ms | Stereo match p50 / p95, ms | Observed processing time / wall |
|---|---:|---:|---:|---:|
| Depth only | 195 / 195 | 304.216 / 324.499 | 292.665 / 311.848 | 99.71% |
| Mixed repeat 1 | 193 / 193 | 308.603 / 320.153 | 296.671 / 307.158 | 99.95% |
| Mixed repeat 2 | 193 / 193 | 309.770 / 326.921 | 297.955 / 314.177 | 100.44% |
| Raw mono + depth | 194 / 194 | 307.378 / 321.297 | 294.957 / 308.720 | 100.01% |

Timing samples deduplicate the actual nested
`matcher_statistics.capture_sequence` within a stable run/epoch. The final
column sums processing time for unique observed completions. A completion can
start before the observed interval, so boundary effects can produce slightly
more than 100%. It estimates the matcher worker's occupancy, not utilization
of all four CPU cores.

The approximately 300 ms matcher work per completion, near-continuous worker
occupancy and similar depth-only/mixed rates support the inference that this
configuration is matcher-compute limited at about 3.2 Hz. The request is 10 Hz;
there is no 1 Hz limit from the deliberately slower earlier test. The data does
not establish that all cores are saturated or that another quality configuration
would have the same rate.

Every raw depth frame included associated mono with matching dimensions and
metadata. Each mixed encoded/depth run had 192 common capture identities with
equal capture timestamps and no mismatches. The supplemental raw case had 194
common mono/depth captures with identical mono SHA256, capture time,
configuration epoch and calibration SHA256, with no mismatches.

There were no stream errors, processing/storage/completion failures, trigger
errors, frame-completion/requeue errors, encoding-pool drops or status errors.
All runs reported `throttled=0x0`. The depth worker deliberately replaced
404–408 pending captures with newer ones while busy; those replacements are
expected bounded scheduling under a 10 Hz request. The status
`capture_sequence_gaps` counter tracks gaps between processed publications and
therefore also advances when depth skips captures. It is not evidence of lost
camera completions in these runs. All such counters are retained in the JSON.

## Restoration and limits

The candidate ran from an extracted artifact with temporary runtime overrides;
installed packages were not replaced. All 73 extracted regular files matched
their staged package payloads, including executable/library hashes. An
independent root restore timer was armed before service changes, and the worker
also had a post-stop restore action. Protected control hashes were checked
periodically; no foreign change was detected.

Independent final verification found all **87/87 protected files unchanged**,
original encoding bytes restored (including its original `depth` source),
original service arguments/activity restored, installed packages unchanged,
temporary overrides removed, worker fully inactive and the restore timer stopped.
No stereo, exposure, calibration, authentication or boot setting was changed.

The exact harness used for these runs is preserved locally as
`.codex-tmp/cleanup098910e20261021_harness_used.py`, SHA256
`83735234d0285dd9b26fd5558859a23e872203854e19856683ac13aa8895001f`.
Subsequent review found a preparation race: a foreign encoding edit while
services stopped could be overwritten before periodic monitoring began. No
such change was detected in these runs, and final 87/87 restoration passed.
The future-use helper now rechecks original bytes after service stop and
immediately before encoding replacement, aborting and restarting original
services while preserving a foreign edit. Nine synthetic tests pass, including
that preparation race and restoration-time edits. The fixed helper's SHA256 is
`9e809060b342fb25f1e5d496770ee1999d62db6adc3149335596924ce6c6b24a`;
it was not the harness used to generate the measurements above.

The two mixed runs are short local throughput checks. They do not replace
endurance, depth-accuracy/calibration, Wi-Fi delivery or browser display testing.
The later beta 5 authentication/installer changes require their own reviewed
package installation and browser checks.
