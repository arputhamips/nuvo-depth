# Beta 5 signed-cohort qualification — 2026-10-21

The exact signed **v0.1.1-beta.5** cohort passed a normal interactive dry-run,
beta 4 → beta 5 Pi upgrade, public same-version installer rerun, automatic-session
API checks and five short Pi-local performance cases. The console required no
Nuvo password, setup code or owner claim. Preview delivery stayed near 10 fresh
packets/s while the unchanged
matcher completed about 3.1–3.2 depth frames/s with both requested at 10 Hz.

This record qualifies source
`0820d50f84c7b4f7c6bb14efe57e79cb2f83b48f`, Debian version `0.1.1~beta.5`,
built and signed by
[workflow 39668603486](https://github.com/arputhamips/nuvo-depth/actions/runs/39668603486).
It is separate from the earlier [098910ed cleaned-runtime test](performance-cleanup-pi-20261021.md).
Full sanitized package hashes, settings, stream/timing distributions, counters,
identity audits and restoration evidence are in the
[qualification JSON](release-qualification-beta5-20261021.json).

The [release](https://github.com/arputhamips/nuvo-depth/releases/tag/v0.1.1-beta.5)
was published at **2026-10-21 20:32:14 UTC** as a prerelease, with latest disabled.
At 20:32:26 UTC, anonymous downloads of all three assets returned HTTP 200 and
matched the reviewed draft's SHA256 hashes. The published assets are the tested
cohort; they were not rebuilt for publication.

The normal interactive Pi rerun through the published, release-pinned curl
installer passed in **20.594 seconds**, exit 0. Its post-install API check passed
at **20:32:57 UTC**, with no browser credential prompt and the same beta 5
packages and protected settings retained.

## Signed artifacts and installed identity

The outer bundle signature and inner manifest signature verified. All 19
manifest files and the eight-package version/architecture cohort matched. The
measured runtime was installed from that verified signed bundle; it did not use
an extracted candidate override.

| Artifact | SHA256 |
|---|---|
| `nuvo-depth-pi5-arm64.tar.gz` | `ca1317044bc28fd8f21b79e5132321675b3c3c52f1b033fe774e7dbdd65992e0` |
| `install-nuvo-depth.sh` | `c1e17e909e3b43ae189a85e0b5af9495052d6018f51503dff5b180ccdbdaa91e` |
| Signed bundle manifest | `5e1fd48fdb5d19040ce8986becc57faf2f4d4acba24c0a51ed8e4d935a631c21` |
| Installed native `depthd` | `3848639c5b9395192842bbb867da6e32801386819a47c47c91a9bde2ca7d9ceb` |
| Installed gateway | `6f49fc8a2a6ff9e36be8592f8b5c1e4e142bc5c17d84962432aed280649a521f` |

Installed payload verification examined 73 regular files across the four runtime
packages: **68 static/runtime files matched exactly**, and five registered dpkg
conffiles were separately hashed. Retained daemon, stereo, exposure and encoding
settings can differ from package defaults; they were preserved. The JSON keeps
both packaged and installed conffile hashes. All eight installed packages had
version `0.1.1~beta.5` and their signed `arm64`/`all` architectures. `dpkg --audit`
was empty, and daemon/gateway services were active.

## Installation and direct access

| Check | Result |
|---|---|
| Normal interactive signed-bundle dry-run | Passed, exit 0, 1.797 s |
| Normal interactive beta 4 → beta 5 upgrade | Passed, exit 0, 20.141 s |
| Normal public pinned curl beta 5 → beta 5 reinstall | Passed, exit 0, 20.594 s |
| Nuvo browser-password question during installation | Absent |
| Installed console handoff | Direct HTTPS access instructions; no code/password step |
| Fresh automatic `GET /api/v1/session` | Passed after upgrade, before publication and after public reinstall |
| Live raw mono without token/code/password claim | Four increasing captures in each API check |
| Secure cookie and invalid-CSRF rejection | Passed |
| Served console HTML | No login controls |
| Identity, calibration, authentication, Wi-Fi and current controls | Preserved; no critical-file differences |
| Published installer/bundle/signature downloads | Anonymous HTTP 200; all three match the reviewed draft hashes |

The immediate post-upgrade check compared 19 critical files; prepublication and
post-public-reinstall checks each compared 22, with no changes. All five
benchmark clients also used automatic GET session bootstrap, validating the returned CSRF token and
Secure/HttpOnly/SameSite=Strict cookie without supplying a claim token, setup
code or password. Existing ownership records remained intact. This confirms
the selected network-access policy: anyone who can reach the HTTPS console can
obtain its administrative browser session. Origin/CSRF/session protections and
the compatible SDK claim interface remain, as described in the
[browser-access implementation report](browser-access-beta5-20261021.md).

The embedded browser could not load the device page because its self-signed
certificate produced **`ERR_CERT_AUTHORITY_INVALID`**. No certificate bypass was
used. Consequently, these results establish certificate-verified API access,
served HTML and the previously passing frontend tests; they do **not** establish
a visually inspected, rendered browser console or displayed/decoded live video.

## Pi-local performance

Hardware was a Raspberry Pi 5 Model B Rev 1.1 with 16 GB RAM, kernel
`6.12.75+rpt-rpi-2712` and the `ondemand` governor. Each case used a five-second
warm-up and a 60-second measured interval. Requests were H264 `left_color`
preview at 10 Hz and a separate raw `depth,left_mono` best-effort request at
10 Hz. The supplemental case used separate raw mono and depth requests.

The existing 728 × 544 profile, SGBM `hh`, 96-pixel search range, disparity bounds
0–96, block size 5, median filter 5 and two OpenCV threads were preserved.
Calibration, exposure, trigger and other controls were preserved. H264 kept its
8 Mbps target, two encoder threads, keyframe interval 1, `veryfast` preset and
low-latency setting. Only the preview source/mode/cadence was temporarily selected
as `left_color`/both/10 Hz. No quality setting was weakened to raise depth FPS.

| Case | Fresh preview/mono FPS | Fresh depth FPS | Native CPU, one-core % | Gateway CPU, one-core % | Maximum °C |
|---|---:|---:|---:|---:|---:|
| Encoded preview only | 9.94929 | — | 19.53 | 0.88 | 53.45 |
| Depth only, requested 10 Hz | — | 3.11184 | 105.06 | 2.56 | 61.15 |
| Preview + depth, repeat 1 | 9.93157 | 3.10145 | 119.44 | 3.48 | 63.35 |
| Preview + depth, repeat 2 | 9.93618 | 3.16967 | 119.72 | 3.51 | 62.80 |
| Raw mono + depth | 9.93750 mono | 3.21122 | 106.13 | 4.77 | 63.35 |

| Encoded preview case | Fresh packets | Interval p50 / p95 / max, ms | Packet latency p50 / p95, ms |
|---|---:|---:|---:|
| Preview only | 597 | 100.490 / 101.065 / 144.935 | 80.988 / 128.397 |
| Mixed repeat 1 | 596 | 100.567 / 103.552 / 147.979 | 89.024 / 131.946 |
| Mixed repeat 2 | 596 | 100.573 / 103.506 / 147.157 | 82.994 / 126.713 |

Relative to preview alone, mixed preview FPS changed by **−0.178% / −0.132%**,
and interval P95 increased by **2.488 / 2.441 ms**. Preview packet-latency P95
changed by +3.549 / −1.684 ms. These short repeated checks show no material
preview-throughput loss for this workload; they do not prove zero contention or
identical latency. Depth FPS changed by −0.334% / +1.858% versus depth alone.

FPS counts unique, increasing device/run/epoch/capture identities after
normalizing raw/encoded UUID spelling. Capture times also had to increase and
remain fresh. Packet latency is capture CLOCK_BOOTTIME to full receipt and SDK
envelope validation over Pi-local certificate-verified TLS. It excludes H264
decoding, browser rendering/display and LAN/Wi-Fi delivery. CPU percentages are
cgroup CPU time/wall time; 100% means one core. Observer/client CPU is recorded
separately in the JSON.

## Matcher work and associated products

| Case | Unique matcher samples / matched counter delta | Processing p50 / p95, ms | Stereo match p50 / p95, ms | Observed processing / wall |
|---|---:|---:|---:|---:|
| Depth only | 186 / 186 | 318.650 / 348.008 | 307.174 / 336.942 | 99.72% |
| Mixed repeat 1 | 186 / 186 | 314.838 / 359.259 | 302.338 / 346.784 | 100.10% |
| Mixed repeat 2 | 190 / 190 | 312.684 / 329.643 | 301.147 / 318.514 | 99.96% |
| Raw mono + depth | 193 / 193 | 308.786 / 327.819 | 297.504 / 315.572 | 100.20% |

Timings deduplicate nested `matcher_statistics.capture_sequence` within one
stable run/epoch. Summed processing time covers observed completed work, which
can start before the interval; boundary effects explain values slightly above
100%. This estimates matcher-worker occupancy, not saturation of all four cores.
The approximately 310 ms processing time, near-continuous worker occupancy and
similar depth-only/mixed rates support the inference that this configuration is
matcher-compute limited at about 3.1–3.2 Hz. The request is 10 Hz; achieved depth
FPS is reported separately.

All raw depth packets contained their associated mono with coherent dimensions
and metadata. Mixed repeats had 185 and 189 common encoded/depth capture
identities with equal timestamps and no mismatches. The supplemental raw case
had 192 common mono/depth captures with identical capture time, mono SHA256,
configuration epoch and calibration SHA256, with no mismatches.

There were no stream errors, processing/storage/completion failures, trigger or
frame-completion/requeue errors, encoding-pool drops or reported status errors.
Every run reported `throttled=0x0`. Capture counters advanced by 599–600 pairs
per measured status interval. The busy matcher deliberately replaced 407–414
pending captures with newer ones. Those bounded scheduling replacements and
the gaps between processed capture sequences remain visible in the JSON; they
are not counted as camera-completion failures. Status and received-packet
interval boundaries can differ by a completion and are kept separate.

## Restoration and acceptance limits

The independent root safety timer was armed before temporary encoding/service
changes. Preparation and restoration rechecked original/owned bytes immediately
before replacing encoding or removing dropins; periodic protected-file hashes
would abort on foreign control edits. Worker finally and post-stop restoration
were also active.

Independent final verification found **99/99 protected files unchanged**,
original encoding bytes restored (including its `depth` source), original
service arguments/activity restored, temporary overrides removed, worker fully
inactive and the restore timer stopped. Packages were unchanged during the
benchmark: the intended beta 5 installation remains installed. The post-upgrade
credential-free API checks still passed afterward.
The later public installer rerun also left the same eight beta 5 versions
installed, both services active, `dpkg --audit` empty and `throttled=0x0`.

This is an upgrade/API/short Pi-local performance qualification on an existing
unit. It does not establish fresh-SD or first-user physical setup, a new hotspot
handoff, visual browser behavior, LAN throughput, endurance, USB power/data,
shipping acceptance or new depth/calibration accuracy. Earlier UF-01/UF-02
records remain evidence for their own cohorts. The source/host validation is
recorded separately in the [implementation report](browser-access-beta5-20261021.md).
