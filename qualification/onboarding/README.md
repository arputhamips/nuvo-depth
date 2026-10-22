# Onboarding acceptance procedures, version 1

The unchanged source specification and 69-case template in this directory define
the requirements. `procedures.json` maps every case to executable coverage and
the remaining operator procedure. Automated fixtures are partial coverage when
the requirement also needs hardware, humans or published pages. Never turn a
passing unit test into a pass for an unexecuted physical subcase.

Implementation owner: Codex. Automated execution/review is identified in each
result. Release and independent acceptance reviewers remain operator assignments;
an agent cannot supply five first-time participants or independent electrical/
metric-accuracy evidence.

Before physical testing, freeze source, artifact hashes, package/signing cohort,
OS image/hash, kernel, dependencies, actual board RAM, camera IDs, calibration
hash/authority, controls, scene, clients and network. Use the adjacent manifest
template. A cleaned existing OS must be recorded as cleaned, never fresh-flashed.
Do not rewrite calibration/device bindings to pass acceptance. Preserve existing
validated per-unit geometry. The shared beta 5 seed remains provisional.

## Execute and retain evidence

1. Copy `acceptance-template.json` to a new candidate record; fill the candidate
   references. `candidate.artifact_manifest` and each passing attempt's matching
   `candidate_manifest` use a path/URL plus SHA-256 evidence object. Copy
   `hardware-manifest-template.json` and freeze conditions.
2. Use commands in `procedures.json`, their stated environment and timeout.
   Record exact command, source, start/end with offset, exit code, output hash,
   observations and criteria. Exit 0 is necessary for an automated test, not
   sufficient for unexecuted manual subcases. Do not run Pi lifecycle procedures
   in a generic host environment.
3. Append every attempt to the case's `runs`. Keep earlier failures after fixes.
   Store evidence objects with local relative `path` or HTTPS `url` and SHA-256.
   Private evidence can stay off-repo. Never publish credentials/private keys.
4. Validate the structure, then request the separate gate explicitly:

```bash
python3 tools/qualification/acceptance.py path/to/results.json
python3 tools/qualification/acceptance.py path/to/results.json --gate software
python3 tools/qualification/acceptance.py path/to/results.json --gate self-service
```

Structural validation can succeed with not-run cases; gates fail closed. Every
software/client/clean-environment/published-doc case is required for the software
gate. All 69 cases and all deliverables are required for self-service acceptance.
Blocked, failed and not-run cases remain visible. Production qualification stays
separate. A URL hash is a frozen declaration; reviewers must download and verify
remote evidence because the validator does not authenticate remote contents.

## Physical lifecycle and network checklist

Use only the designated recoverable Pi with local recovery access. Record state
hashes without exposing secrets, and restore test-only settings afterward. The
test operator's current reset authorization determines backup handling; this
run explicitly waived backups and retained protected calibration/identity.

For each profile, use its normal installation flow on a newly flashed supported
image, then same-profile signed-cohort reinstall/upgrade. Request a real depth/
validity reader after required reboot. Record advancing identity, metadata,
authority, package audit and protected-file hashes. The cleaned-system trial is
additional evidence and cannot satisfy a fresh-flash case.

Perform ten graceful reboots and five physical power removals per profile.

The operator host can run `tools/qualification/pi_reboot_test.py --help` (install
`paramiko` on that host). Supply verified known-hosts, host/address/user, profile,
a new output directory and append-only command audit. It prompts privately,
performs actual kernel reboots, checks new boot/run identities, advancing paired
output, protected hashes and package audit. It cannot remove physical power.
Each cycle needs pre/post state hashes, distinct daemon run, fresh reader output,
service status and a timestamped operator power event. SSH reboot is not a power
cycle. Never automate physical power removal without the appropriate switch.
Use an isolated recoverable activation for interruption tests and retain all
transaction receipts, including restored policy after failure.

Keep Ethernet SSH available while testing Wi-Fi. Run twenty handoffs, four each:
correct credentials/confirmation; wrong credentials; no confirmation until the
existing timeout; reboot before confirmation; confirmed network then outage.
Record phase/status, selected path and hashes of protected hotspot/network
material before/after, never actual secrets or confirmation links. Unconfirmed
handoffs must recover the same protected hotspot; confirmed ordinary outages
must not reopen it. Use existing Connections controls and record restoration.

## Frozen performance protocol

`performance-thresholds.json` freezes ratios before measurement: depth rate
>=95% baseline, end-to-end p95 <=110%, steady service RSS <=110%; retain any
stronger existing absolute product threshold. Two OpenCV threads and CPU
rectification remain unchanged. Compare the same geometry, algorithm/controls,
OS/dependencies, scene/lighting, hardware, consumers and measured network.

For each full/minimal core-depth, mixed monitoring and bounded multi-client
workload, warm for five minutes, then measure three ten-minute runs for each
baseline/candidate. Perform a separate sixty-minute soak per retained shipping
workload. Save unique device/run/capture identities, capture/processing/receive
timestamps, stage counters, bytes, RSS/CPU, swap, temperature/throttle and errors.
Reject mismatched controls/cohorts. Extend noisy comparisons instead of choosing
favorable samples. Physical SKU claims require that actual board, not firmware
memory caps. Golden stereo comparison requires recorded left/right input and
predeclared equivalence tolerances; a decoded depth-only fixture is insufficient.

Use `tools/qualification/observe_runtime.py` for bounded resource samples and
`compare_performance.py` for frozen-budget comparison. They observe; they do not
create network clients or establish power, SKU or accuracy evidence. Start the
documented consumers separately and retain their identity/timestamp trace.

For the local depth/validity workload, `measure_local_depth.py` runs the existing
bounded native reader, verifies advancing associated identities, measures latency
against the declared boottime capture clock, and retains per-frame traces plus
resource samples. Supply frozen source/artifact/condition SHA-256 and a conditions
JSON containing `protected_hashes`, `calibration_sha256`, `calibration_authority`
and the remaining physical controls. It rejects control/calibration drift. The
default is three runs, each with 300-second warmup and 600-second measurement.
Use `--runs 1 --warmup 300 --seconds 3600` for a separate hour of this workload.
Network/monitoring/multi-client workloads still need their declared consumers;
this tool cannot stand in for them. Short smoke invocations do not pass budgets.
