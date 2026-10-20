# Preview and depth requested at 10 Hz on the Pi — 2026-10-20

Both subscriptions requested 10 Hz. The encoded camera preview stayed at **9.94 fresh frames/s**, while depth delivered **3.22–3.23 fresh results/s** with the unchanged SGBM `hh` settings. The two mixed runs showed essentially identical preview throughput to preview alone, with interframe p95 increasing by 1.2–2.8 ms. No runtime source change was needed for these valid tests.

Each case used 5 seconds of warmup followed by 60 measured seconds over Pi-local, certificate-verified TLS. The preview request was `/api/v1/encoded/stream?cadence_hz=10`, using H264 `left_color`. The separate raw request was `/api/v1/stream?products=depth,left_mono&cadence_hz=10&delivery=best_effort`. FPS counts increasing, unique capture identities; it does not count repeated cached results.

| Pi-local case | Preview FPS | Depth delivered FPS | Native matched frames / Hz | Capture Hz | Daemon / gateway CPU |
|---|---:|---:|---:|---:|---:|
| Preview alone | 9.94394 | — | 0 / 0 | 9.988 | 17.87% / 0.66% |
| Preview + depth, repeat 1 | 9.94497 | 3.21972 | 193 / 3.21843 | 9.988 | 117.90% / 3.25% |
| Preview + depth, repeat 2 | 9.94421 | 3.23243 | 193 / 3.21988 | 9.993 | 118.71% / 3.37% |

CPU uses one core as 100%; the Pi has four cores. The daemon cgroup includes its encoder children. The matcher counter is independent of encoded/raw publication totals. Counter windows use sampled status endpoints, so their approximately 59.96-second intervals differ slightly from the 60-second subscriber windows.

| Case / stream | Fresh packets | Interframe ms, p50 / p95 / max | Packet latency ms, p50 / p95 / max |
|---|---:|---:|---:|
| Preview alone | 597 | 100.3 / 140.7 / 148.0 | 92.9 / 157.5 / 174.1 |
| Mixed repeat 1 / preview | 596 | 100.5 / 141.9 / 148.3 | 86.6 / 130.2 / 171.8 |
| Mixed repeat 1 / depth + mono | 193 | 308.7 / 340.0 / 363.8 | 397.8 / 446.0 / 479.9 |
| Mixed repeat 2 / preview | 597 | 100.5 / 143.5 / 150.4 | 85.3 / 156.6 / 183.0 |
| Mixed repeat 2 / depth + mono | 194 | 307.7 / 318.1 / 355.0 | 393.6 / 444.9 / 475.6 |

Latency measures the capture CLOCK_BOOTTIME timestamp to the fully received packet with its SDK envelope decoded/validated. For preview, the H264 payload was received and its NAL structure inspected. H264 picture decode, browser rendering and display latency were not measured. The Pi clock is shared by capture and the local observer. FPS is `(unique packets − 1) / (last arrival − first arrival)`.

| Mixed case versus preview alone | Preview FPS change | Interframe p95 change | Preview packet latency p95 change |
|---|---:|---:|---:|
| Repeat 1 | +0.0103% | +1.18 ms | −27.32 ms |
| Repeat 2 | +0.0027% | +2.81 ms | −0.94 ms |

Depth ran at the completion rate of the current matcher, with continuous 10 Hz demand and replacement of pending captures while it was busy. These results show that a 10 Hz depth request is not limited to the 1 Hz cadence used in the earlier test. They do not establish 10 new depth results/s or saturation of all four CPU cores.

**Controls and provenance.** Raspberry Pi 5 Model B Rev 1.1, 16 GiB, Debian 13/Trixie, kernel `6.12.75+rpt-rpi-2712`, `ondemand` governor, dual IMX296, 728×544 capture at 10 Hz. The valid runs preserved SGBM `hh`, search/max 96, min 0, block 5, OpenCV threads 2, median 5, uniqueness 0, speckle 30/2, prefilter 31, specular 256 and disparity-12 max difference 1. Calibration and exposure were unchanged. Preview retained native 728×544 H264, 4 Mbps, `veryfast`, threads 2, low latency and keyframe interval 1. The temporary encoding mode changed from `encoded` to `both`.

The ARM64 packages came from [Actions run 39465250004](https://github.com/arputhamips/nuvo-depth/actions/runs/39465250004), source `c5eef0b68b65d0d66d6334ad42fc4c2915a628f1`. Repository HEAD was `e7df137ab088e8e3361e03bf0c5adf6f2226e931`; its apps/src/platform/include/SDK/web runtime source is identical. All four package SHA256 checksums and all 86 extracted regular files were verified against package contents. Executable/library hashes are in the JSON. Testing used extracted artifacts and temporary service overrides, without installing packages or creating a tag/release.

**Matcher timing and integrity.** The early Pi harness retained only the final matcher stage sample because it keyed a nonexistent top-level sequence; the actual key is nested `matcher_statistics.capture_sequence`. The following values are individual final observations, **not timing distributions**. Stream rates, native counters, CPU, freshness, interframe and packet-latency distributions were unaffected. The aggregation was corrected and checked with synthetic changing/duplicate nested sequences; optional follow-up measurements were stopped after external control activity.

| Mixed case | Total ms | Rectification ms | Preprocess ms | Match ms | Postprocess ms |
|---|---:|---:|---:|---:|---:|
| Repeat 1, final sample | 309.496 | 5.184 | 0.0019 | 297.548 | 3.660 |
| Repeat 2, final sample | 306.688 | 3.976 | 0.0026 | 295.880 | 3.608 |

All five measured streams had unique increasing capture IDs and increasing capture timestamps, with no stream errors. Each raw packet contained both `depth` and `left_mono` at 728×544 with consistent frame timestamps and descriptors. Encoded/depth capture epoch and sequence overlaps had equal capture timestamps: 192 common captures in repeat 1 and 193 in repeat 2, with zero mismatches. Canonical encoded UUID strings and compact raw UUID strings were hashed separately; overlap checks used epoch/sequence within the same uninterrupted daemon run, then compared capture timestamps.

| Case | Pending matcher replacements | Storage / protected / completed drops | Maximum temperature | Throttling |
|---|---:|---:|---:|---|
| Preview alone | 0 | 0 / 0 / 0 | 50.15°C | `0x0` |
| Mixed repeat 1 | 406 | 0 / 0 / 0 | 55.10°C | `0x0` |
| Mixed repeat 2 | 405 | 0 / 0 / 0 | 56.20°C | `0x0` |

In these three valid runs, camera completion, trigger, requeue, processing, exposure, encoder-pool, stale-pair and capacity error/drop counters were zero. Pending replacements and raw `capture_sequence_gaps` reflect choosing newer captures for a slower matcher; the native capture and encoded counters advanced by 599 in each mixed status window.

**Interruption and coverage limits.** The required baseline and two mixed runs completed before unrelated Pi control changes. Estimated last packet arrivals were 18:58:34.391, 18:59:40.461 and 19:00:46.657 UTC. At 19:01:00 UTC, the setup authentication file changed with an authentication failure recorded. At 19:01:31 UTC, stereo mode changed externally from `hh` to `3way`. The harness never writes either file. Its supplemental raw mono-10 + depth-10 case disconnected during this activity and was excluded. The independent reviewer reported no Pi activity from its chat; the external actor was not identified by this test.

Windows network measurements were **not started** after this interference was detected. This report makes no Windows delivered-throughput claim. The interrupted raw mono/depth case does not establish independent image-worker throughput under saturation. A subsequent network/raw test needs an idle Pi and a fresh snapshot of its current controls. Depth accuracy was not revalidated; the device's existing provisional calibration remained in place throughout the valid runs.

**Restoration.** An independent 20-minute restore timer was armed before mutations. The Pi worker also had a 360-second runtime limit and ExecStopPost restoration. Final verification confirmed exact original encoding bytes, ownership/mode, installed service arguments, service active states and package versions restored; temporary overrides were removed. Of 87 protected files, **85 were unchanged**. The two external stereo/auth changes were preserved, including the resulting `3way` mode. The worker was no longer running, and its independent timer was stopped/removed before it could overwrite later activity. The encrypted local credential was removed after remote verification. Installer, release, rollback code and existing reports were untouched.

Sanitized machine-readable evidence: [performance-preview-depth-10hz-20261020.json](performance-preview-depth-10hz-20261020.json). It includes exact counters, settings, artifact hashes, frame-audit hashes, baseline comparisons, interruption and restoration evidence; full frame records remain in the listed ignored local evidence file.
