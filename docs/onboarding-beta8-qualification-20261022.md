# Beta 8 onboarding qualification — closeout

Updated 23 October 2026. Long tests stopped at the operator’s request at 08:00 UTC. Frozen runtime/package source:
[`e7cc0fa63f0bd38d16c6b407b4f7304a747759df`](https://github.com/arputhamips/nuvo-depth/tree/e7cc0fa63f0bd38d16c6b407b4f7304a747759df).
Subsequent report/changelog edits are documentation only. The approved public
preview remains **beta 6**. Beta 8 is not publicly approved and no completion
gate is declared complete. The website is published at
[Start Here](https://docs.nuvohardware.com/vision-one/guide/getting-started).

## Implemented behavior

The candidate provides shared installer/console/CLI readiness; transactional
calibration import, dry-run and rollback on full and minimal; durable activation
recovery and fresh-output verification; read-only allowlisted support exports;
version/recovery guidance; six Python examples and a public C decoder example;
and documented clean host builds with required CI coverage.

Minimal upgrades preserve the previous effective policy before replacing the
service unit, including a stopped beta-6 implicit-preview service. A deliberate
disable stays disabled despite leftover validated health. New minimal installs
retain their documented provisional-preview default; full setup explains and
requests its preview choice. Existing per-unit bundles are preserved. Shared
beta-5 geometry remains authority 0 provisional; software upgrades alone do not
require recalibration or establish independent metric accuracy.

Rolling GitHub installers resolve one centrally approved public reference.
Release-specific scripts remain pinned. Draft/development versions cannot
silently advance the recommendation; future approved updates do not require
rewriting the rolling script or current README commands.

## Frozen-source evidence completed

| Coverage | Observed result |
| --- | --- |
| Hosted candidate matrix | All 11 jobs pass: minimal/full, clean Debian, minimum CMake, browser and Linux/Windows SDK jobs. |
| Fresh Ubuntu 24.04 WSL | Documented bootstrap only; 49/49 minimal and 67/67 full CTest entries pass. |
| CMake versions | 3.25.2 and a newer supported host version pass; 3.24.3 rejects with the stated minimum. |
| Installed public C decoder | Compiles outside the checkout against installed headers/library and decodes an actual frame. |
| Console fixtures | Ten first-use/readiness checks and eight real-gateway browser journeys pass. |
| Actual Pi clients | Fresh Windows Python 3.11/3.13 and clean Linux Python 3.12 run six examples and two product-format sets. |
| SDK lifecycle | 32 short claim/close cycles per physical client, 96 total, pass while another client stays authorized. |
| Actual keyboard tasks | Desktop light and 390-pixel mobile light/dark complete first-frame save, diagnostics refresh/download, version navigation and advanced-control toggling using Tab/Enter only; no page errors. |
| Read-only diagnostics | All 134 checked persistent files remain unchanged. A stopped service with a recent restored cache cannot establish loaded/fresh verification. |
| Interrupted package work | Exact-candidate core unpack, installer refusal, documented dpkg repair and normal interactive retry pass; protected state and fresh provisional output return. |
| Minimal lifecycle | Normal GitHub bootstrap retry produces float depth, validity, metadata and associated NUV output; NumPy, Python OpenCV and the network SDK are absent. Same-profile stopped-preview reinstall, explicit-disable preservation against stale accepted cache, interrupted dpkg repair, signed beta-7 upgrade and one closeout reboot pass. Initial cross-profile fixture failures remain retained. |
| Full-profile reboots | Ten graceful kernel reboots pass with new boot/run IDs, advancing paired output, unchanged protected settings and clean dpkg audit; 28–69 seconds each including verification. |
| Physical setup Wi-Fi | Twenty handoffs pass, four each for success, wrong password, missed confirmation, reboot during handoff and confirmed outage; same protected hotspot secret, zero client reconnect retries. |
| Payload equivalence | Fixed stereo input gives exactly equal disparity/depth/validity/confidence/rectified payloads against signed beta 7 with unchanged geometry/controls. |
| CI controls/history | Deliberate native/SDK/UI regressions fail the relevant jobs and aggregator; 43 historical task-base documents remain byte-identical; generated/private source paths are excluded. |

Workflows:
[normal matrix](https://github.com/arputhamips/nuvo-depth/actions/runs/39887740868),
[minimal ARM64 artifacts](https://github.com/arputhamips/nuvo-depth/actions/runs/39887845193),
[full ARM64 artifacts](https://github.com/arputhamips/nuvo-depth/actions/runs/39887847648).
The physical candidate uses explicitly selected controlled test release signing.
That key grants neither public-release approval nor calibration trust. Official signing, artifact parity and final installation are recorded in the
final private evidence index. The versioned beta candidate remains a draft
prerelease; public approval stays at beta 6.

## Physical environment and transport scope

The operator selected cleanup of the existing OS and waived a new backup. Existing
home contents and application/development packages were removed while retaining
OS networking/SSH/camera support and protected identity/calibration. This is
**cleaned existing Raspberry Pi OS**, not fresh-flash or factory-state acceptance.
The board is Pi 5 Rev 1.1, revision `e04171`, physical **16 GB**, without a memory
cap; dual IMX296 cameras; Trixie userspace; kernel `6.12.75+rpt-rpi-2712`.
Smaller physical RAM SKUs remain unqualified for this candidate.

The initial cleaned-system public GitHub minimal install selected and verified
beta 6, rebooted and produced float depth, validity, metadata and NUV streams.
Its stopped implicit-preview upgrade to an earlier beta-8 implementation worked.
These are supporting historical attempts, not replacements for frozen-candidate
minimal lifecycle evidence. The original bundle binds another unit identity;
both identities and bundle bytes remain preserved. The operator explicitly
deferred that history and requested preview-only testing.

Fourteen separate actual-interface admission probes pass. Ethernet best-effort
1/2/4/10/30 Hz requests are admitted and 60 Hz rejected; Wi-Fi admits tested
1/2/4 Hz requests and rejects 10/30/60 Hz. Exact 1 Hz requests are rejected for
this unqualified source. Admitted cadence is a maximum: the Wi-Fi 4 Hz sample
receives about 2.5 frames/s with associated identities preserved.

Separate synthetic TLS payload measurements use five-second warmup and sixty-second
measurement, observing approximately **118 MB/s Ethernet** and **5.1 MB/s setup
Wi-Fi**. Payload is discarded. These are link observations, not camera throughput
or universal capacity guarantees. Gateway resource budgets remain 64 MiB/s wired
and 8 MiB/s Wi-Fi, explicitly unqualified as measured capacity. No exact-delivery
qualification is enabled. Begin Wi-Fi raw depth at 1 Hz best effort. The original
OS station/keep-current policy is restored, the test station profile removed,
protected settings unchanged, and the temporary TLS probe listener closed.

## Issues and failed attempts retained

| Finding | Handling |
| --- | --- |
| Minimal upgrade lost implicit preview or used cached accepted health to re-enable disabled capture | Snapshot old policy before unit replacement; remove cached-authority fallback; preserve explicit choices. |
| Installer import bypassed verified activation | Shared validation/transaction, rollback and explicit reboot-pending verification. |
| Stopped/stale/malformed state misrepresented readiness | Typed state and real service-activity guard; keep camera health separate from declared calibration. |
| Fixed temporary config filename stranded crash retries | Unique private temporary writes; actual subprocess-crash and ENOSPC tests preserve committed content. |
| Short SDK commands exhausted the 16-session pool | Reuse valid slots, revoke only the closing client, close examples in finally, and retain another active client's access. |
| Session HTTP 503 incorrectly blamed capture | Specific session-capacity recovery guidance. |
| Pi IPv6 path stalled public-release discovery | Bounded curl Happy Eyeballs, HTTPS/size limits and unchanged signature verification; no networking rewrite. |
| Standard os-release symlink reported unknown | Allow only its exact standard target; foreign/chained links remain rejected. |
| Host prerequisites/CMake/current setup instructions drifted | Clean bootstrap, aligned minimum, centralized release/platform/profile checks and published wiki updates. |
| Old beta-6 SDK lacks example module/remote cleanup | Document current-source clients and legacy session recovery; do not rewrite old release assets. |
| Preserved accepted bundle names another unit | Preserve it and the Pi identity; accepted physical import awaits the deferred identity history. |
| Backward reset to beta 6 with future B5 configuration failed | Downgrade/geometry guards refuse it; retain failure, do not advertise an untested rollback. |

Environment/harness failures remain separately recorded: slow Actions APT mirror,
unsupported warm-host Boost setup, UV venvs without pip, an earlier cohort's failed
Windows hotspot rejoin, an expected WebSocket rejection misclassified by the first
transport harness, and CRLF in the first stopped-cache wrapper. Corrected attempts
have distinct records; failures are not erased or relabeled as candidate passes.

## Performance result and operator stop

The full-profile core workload completed three ten-minute baseline and three
ten-minute candidate measurements, each after five-minute warmup, followed by a
separate one-hour candidate soak after warmup. Matched CPU rectification,
Balanced matcher, two OpenCV threads, B5 geometry, exposure/gain, OS/dependencies
and declared consumers were unchanged. This compares extracted signed-beta-7
C++ components with the candidate on the same current helper/OS environment;
it is not a comparison of two complete historical OS installations.

| Frozen core check | Observed result | Limit |
| --- | --- | --- |
| Conservative depth-rate ratio | 0.99983; candidate 9.9983–10 Hz | ≥0.95 |
| Conservative end-to-end p95 ratio | 1.01265; candidate 122.78–124.87 ms | ≤1.10 |
| Aggregate service steady RSS ratio | 0.99940 | ≤1.10 |
| Soak last/first ten-minute aggregate RSS | 1.000086 | ≤1.10 |
| Soak fitted aggregate RSS growth/hour | 13,938 bytes | ≤1,900,380 bytes |
| Soak depth output | 35,997 frames; 9.9992 Hz; p95 126.22 ms | Supporting observation |
| Swap, throttle, new core errors, unexpected service/capture restarts | None observed | None allowed |

The declared **core** comparison/soak passes its frozen limits. This does not
establish all-workload endurance or a universal throughput claim. The operator
requested stopping long tests at 08:00 UTC. The active mixed-client run received
SIGTERM, exited 143 as expected and restored installed candidate bytes and
encoding. Partial mixed-client results are not qualification. Remaining bounded
multi-client and minimal long comparisons/soaks were not run. Three earlier
30-second full workload smokes remain supporting checks only. An earlier long
attempt interrupted by a Pi restart/network loss is retained separately.

## Additional closeout issues

The controlled full-to-minimal switch exposed retained full service overrides,
an empty root-only calibration namespace and a gateway process surviving package
removal. The original attempt failed. Isolating those operator-fixture leftovers,
stopping the old process and deliberately enabling preview allowed the minimal
output/reinstall checks to pass. An orphan Python namespace also caused an
overstrict dependency harness check; no SDK client module, NumPy or cv2 remained.
These recovery attempts do not establish a clean new-package default pass on
the frozen candidate. Same-profile upgrades pass independently. Full-to-minimal
migration is explicitly unsupported in the current installation guides.

The optional signed-beta-6 **service-unit-only** fixture failed to produce fresh
frames with the current runtime/configuration. Its original service/policy were
restored. This is a failed operator fixture, not a successful whole-beta-6
upgrade. Host preinst regression coverage and the real signed-beta-7 upgrades
remain separately identified. The preferred full profile and its original
protected files are restored before closeout.

## Evidence and acceptance limits

Private `output/spec-implementation-20261022` retains the frozen candidate
manifest, official signature/package/source comparison, environments, frame
metadata, browser traces, every failed and corrected attempt, harness sources,
the final unchanged 69-case record and SHA-256 evidence index. The closeout
records 53 passing cases, 15 blocked cases and one failed removal/profile-switch
case; 22 deliverables are complete and two performance reports remain partial. The append-only
`pi-commands.jsonl`, rendered `pi-command-ledger.md` and
`pi-command-executions.csv` retain every audited Pi bash invocation, including
failures and repeated probes; child process argument logs accompany harnesses.
Credentials, private keys and raw private imagery are not published in Git.

[Procedures and forms](../qualification/onboarding/README.md) retain all 69 cases
and 24 deliverables. The final record keeps blocked and unfinished requirements
visible. Software implementation and self-service acceptance are separate
gates; structural validity alone does not grant acceptance. No complete
self-service or production qualification is claimed.

Freshly flashed storage, five physical power removals per profile, five independent
first-time users and a matching validated bundle were not supplied. Physical
missing-camera/unsupported-kernel subcases, smaller physical RAM SKUs, remaining
long workloads and the full minimal reboot sequence remain unqualified.
The operator chose cleaned-system testing and later stopped long testing;
these instructions are recorded without weakening the frozen acceptance rules.
Agent walkthroughs, SSH reboots, memory caps and synthetic accepted bundles do
not replace those cases. No new accuracy, usable-range, universal rate or
smaller-RAM claim is made. Identity history remains deferred by the operator.
