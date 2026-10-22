# Nuvo Depth Onboarding Implementation Specification

Version 1.0 · 22 October 2026 · Status: planned work

This specification covers all seven improvements from the repository review, including the previously planned documentation and calibration work, plus the performance recommendations. It defines the implementation deliverables, required outcomes, and verification evidence for an easier first setup and reliable continued use.

The intended outcome is that a newcomer can select a supported setup path, install the software, understand calibration readiness, obtain a fresh associated frame, and run an application example without maintainer intervention. Existing capture, configuration, SDK, recording, networking, and package behavior must remain compatible with the supported contracts.

## Scope and completion rules

The reference source is [commit 6537e2c](https://github.com/arputhamips/nuvo-depth/tree/6537e2cafbd154a70c0d044097c4d3e1f52803f1). The approved public preview at the review date is v0.1.1-beta.6. A later implementation must record its own frozen source revision and exact package identities.

In scope: setup documentation; browser and CLI readiness; supported calibration import; runnable SDK examples; actionable diagnostics and upgrade guidance; reproducible host development; automated regression coverage; physical onboarding acceptance; and measured performance defaults.

The full appliance and minimal raw-depth installation remain separate, mutually exclusive profiles. Minimal continues to omit the web console, network SDK, Wi-Fi onboarding, calibration solver, and monitoring encoder. The calibration import feature must not pull those features into minimal.

New camera platforms, a complete ROS 2 bridge, a new browser updater, factory reset, cloud services, and changes to stereo algorithms are separate projects. Public C integration examples are included. ROS 2 support must be labeled according to its actual implementation.

Two completion gates apply:

1. **Software implementation complete:** all deliverables exist, required automated checks pass for the frozen revision, and no unresolved defect blocks an advertised workflow or violates an existing supported contract.
2. **Self-service beta accepted:** the exact candidate artifacts pass the required physical installation, lifecycle, network, performance, and newcomer walkthrough tests. Every retained hardware/RAM claim has matching physical evidence.

Production metric-accuracy and factory-shipping claims require the separate electrical, per-unit accuracy, and manufacturing qualification gates. Completing onboarding does not grant those claims.

All test cases below start as `not_run`. A passing historical release report or a baseline host run cannot be substituted for candidate evidence. No finite test plan proves the absence of every defect; these gates define the required regression coverage and the supported scope.

## Delivery order and responsibilities

| Change | Scope | Dependency | Required evidence |
| --- | --- | --- | --- |
| A | Step 2 consistency and shared public-install references | None | Documentation checks and unchanged historical records |
| B | Step 3 readiness model and consistent UI/CLI messages | A | State, API compatibility, and browser tests |
| C | Step 3 transactional calibration import | B | Validation, interruption recovery, package isolation, and Pi activation |
| D | Step 1 complete first-use paths | A and B; final calibration instructions require C | Walkthroughs, screenshots, executable commands |
| E | Step 4 SDK quickstarts and examples | B and C | Fresh client environments, real TLS integration, recordings |
| F | Step 5 diagnostics and upgrade guidance | B | Failure injection, redaction, lifecycle evidence |
| G | Step 6 reproducible development | None; updated for all changed dependencies | Clean host builds and dependency checks |
| H | Step 7 CI and physical acceptance | Begin with A and G; integrate each change | Required CI jobs and frozen-artifact physical reports |
| I | Performance defaults and final qualification | B, E, F, and H | Controlled baseline/candidate measurements |

Assign an implementation owner and an acceptance reviewer to each change before work starts. The release maintainer owns public-version selection and publication decisions; a hardware operator records physical evidence; a usability observer runs newcomer sessions. One person may fill multiple roles, but an automated report cannot supply missing human or hardware observations.

## Step 1 Complete first-use journeys

### Required behavior

Provide one clearly visible Start Here entry with three paths: preinstalled Vision One, user-supplied Pi/SD card, and application integration. Each path states prerequisites, supported hardware/software, required credentials, the exact starting point, and an observable finishing condition.

For a user-supplied Pi, recommend the full appliance for browser-led use and explain minimal as local raw depth over an already configured SSH/network connection. Show unsupported configurations before installation. Link the exact supported OS image and explain kernel compatibility without suggesting an unqualified system upgrade.

The walkthrough must include installation completion, reboot/reconnect, device discovery and IP fallback, certificate verification, readiness, fresh output, and the first saved or consumed frame. Show real screenshots and expected output. Explain that an idle demand-driven matcher can be healthy at zero depth work.

### Deliverables

- D1-01: Start Here page and aligned README entry linking the three paths.
- D1-02: Complete full/minimal setup walkthroughs, prerequisite checklist, screenshots, and symptom-based recovery links.
- D1-03: A newcomer walkthrough script and observation form recording total elapsed time, active task time, download/boot delays, failures, and interventions.

### Acceptance tests

| ID | Setup and procedure | Required result | Evidence |
| --- | --- | --- | --- |
| T1-01 | A newcomer chooses a path for a preinstalled unit, own Pi with browser, own Pi with local raw output, and remote application. | Each scenario reaches the appropriate path; minimal omissions are visible before installation. | Navigation checklist and screenshots. |
| T1-02 | Follow full setup on a freshly flashed supported Pi using only the user guide; include one reboot and a depth/validity reader. | Console loads, readiness is explained, fresh paired output advances, and the frame metadata identifies the actual profile and authority. | Screen recording, sanitized transcript, frame metadata. |
| T1-03 | Follow minimal setup from an SSH-ready supported Pi and capture a float-depth frame. | Depth, validity, and metadata files are created with documented dimensions/units; the guide never asks for a browser or network SDK. | Transcript, file checksums, decoded metadata. |
| T1-04 | Disable mDNS resolution, then follow the documented IP fallback and certificate instructions. | The user reaches the correct device and verifies its identity; any certificate hostname mismatch has a supported resolution without disabling TLS checks in SDK code. | Browser and client verification record. |
| T1-05 | Begin from an unsupported OS/kernel, one missing camera, and an interrupted install in separate trials. | Instructions identify the condition, state a specific recovery step, and never report successful capture or metric accuracy. | Error screenshots and recovery transcript. |
| T1-06 | Observe five first-time users: two preinstalled, two own-Pi full, and one SDK integration. Supply working supported hardware, approved OS where applicable, internet, and trusted setup material. | All five finish their assigned first-frame task without maintainer hints. Active task-time targets: 15 minutes preinstalled, 30 minutes own-Pi full, 15 minutes SDK. Report total time separately; physical assembly, OS flashing, and new calibration acquisition are outside these timed tasks. | Individual task forms, timings, interventions, observed frame identity. |

### Final result

Users follow one coherent path to a fresh frame and can state what is ready, what remains provisional, and how to recover from the documented common failures. If the timing or assistance target fails, fix the workflow and rerun with new participants; do not relabel the failed session as a pass.

## Step 2 Keep current instructions consistent

### Required behavior

Maintain one explicitly approved public-install reference containing the recommended published tag and matching profile assets. This value is deliberately selected by the release maintainer. Do not derive it from the newest development changelog entry or a moving latest/main alias. Reuse existing platform metadata for prerequisites and the profile catalog for the starting matcher recommendation.

Current user-facing pages must agree about release selection, port 7443, automatic browser sessions, the SDK's separate token flow, full/minimal capabilities, Balanced as the current default, and the distinction between targets and measured throughput. Historical reports retain their original versioned commands and context.

Render or check only the applicable current documentation blocks. Context-aware checks must allow valid SDK uses of claim/token and intentionally historical ports/versions. Include corresponding wiki updates when the wiki is the published user entry point; a repository-only change does not complete this step.

### Deliverables

- D2-01: Public-install metadata and a repeatable renderer/checker reusing platform and profile sources.
- D2-02: Updated current README, installer/user references, console copy, and published wiki content.
- D2-03: Lightweight CI consistency checks with explicit historical-document exclusions.

### Acceptance tests

| ID | Setup and procedure | Required result | Evidence |
| --- | --- | --- | --- |
| T2-01 | Render/check current instructions for full and minimal using the approved public-install metadata. | Every current install command names the approved published tag and the correct signed profile asset; source-checkout instructions still require an explicit release where appropriate. | Generated diff and consistency report. |
| T2-02 | Add a future unpublished development version to a fixture without changing public-install metadata. | Current user commands remain pinned to the approved public preview. Missing/invalid public metadata causes an actionable check failure. | Fixture check results. |
| T2-03 | Compare OS/kernel and starting-profile instructions with platform metadata and the authoritative profile catalog. | Values agree; all rate claims retain their qualification labels and targets remain labeled as targets. | Consistency report. |
| T2-04 | Walk through current browser and SDK connection guidance. | Browser users are not asked to claim/login; SDK token exchange remains documented and functional; terminology distinguishes hotspot, SSH, and SDK credentials. | Copy review and SDK connection smoke result. |
| T2-05 | Compare historical reports before and after rendering/checking current documentation. | Historical version references, evidence, commands, and measurements remain unchanged. | Scoped diff/hash report. |
| T2-06 | Open published wiki pages and their linked current instructions after update; exercise documentation-only CI. | The served pages agree with the repository and links resolve. Only lightweight checks run; ARM64 packaging/signing is not dispatched. | Live-page review and workflow run. |

### Final result

A newcomer receives the same release, platform, connection, and starting-profile guidance at every current entry point. Public installation references cannot drift merely because development advances.

## Step 3 Make calibration readiness clear and import supported bundles

### Required behavior

Define a shared readiness contract used by the installer/setup status, daemon-facing services, CLI, and browser. Add fields compatibly; preserve existing API fields and calibration-authority values. Readiness must reflect validated active state, not merely an old installer flag. Preserve the authority, source, bundle/hash, hardware/profile bindings, reason, and next action needed to explain the result.

| State | User message | Next action |
| --- | --- | --- |
| Calibration accepted | A matching calibration passed its declared validation. | Review its scope and verify fresh output. |
| Preview enabled | Output is available; distance accuracy is unverified. | Continue evaluation or import accepted calibration. |
| Calibration needed | Capture is waiting for calibration or an allowed explicit preview selection. | Import a matching bundle or choose preview on supported hardware. |
| Status unavailable or error | Readiness could not be established. | Show the specific cause and recovery action. |

Maintain camera/service health and calibration readiness separately. Stopped capture, stale status, malformed state, and disconnected cameras must never appear as newly accepted calibration. Declared geometry validation alone cannot produce a production metric-accuracy claim.

The full installer retains explicit engineering-preview opt-in and its existing capture gating. Minimal retains its documented provisional behavior with an unmistakable preview notice; upgrades must not silently change existing defaults or break unattended commands. Custom/incomplete hardware must not inherit assumed capture geometry.

Explain the outcome of each installer selection before applying it. Show the same readiness summary after install and in Overview, calibration details, relevant live-output views, and CLI status. Keep solver geometry and quality controls available to trained operators behind advanced disclosure.

Provide these proposed CLI operations on supported profiles:

```bash
sudo nuvoctl calibration status
sudo nuvoctl calibration import /path/to/bundle --dry-run
sudo nuvoctl calibration import /path/to/bundle
sudo nuvoctl calibration rollback
```

Reuse existing integrity, identity, namespace, signature, activation, and recovery rules. Minimal may use a small shared validation/activation helper, but must not gain solver or network dependencies. Existing saved bundles, CLI forms, and frame authority metadata remain compatible.

Dry-run validates without changing persistent calibration, service selection, or runtime behavior. Import stages the verified bundle, serializes activation, preserves the previous selection, and confirms the daemon loaded the expected bundle/hash. A fresh requested frame must agree before reporting the activation as verified. Reboot-required activation reports pending verification. Failed/interrupted activation restores the prior committed selection and reports any independent camera/service error. Never silently downgrade authority or rewrite device/camera identities to make a bundle fit.

### Deliverables

- D3-01: Shared readiness schema, derivation rules, API/CLI output, installer summaries, and consistent console presentation.
- D3-02: Supported calibration status/import/dry-run/rollback commands, transactional service selection, and compatible bundle validation.
- D3-03: Full/minimal migration guidance, calibration provenance explanation, and interruption-recovery tests.

### Acceptance tests

| ID | Setup and procedure | Required result | Evidence |
| --- | --- | --- | --- |
| T3-01 | Exercise accepted, preview, needed, unavailable, malformed, and stale state fixtures. | Every interface derives the correct state/reason/action; unknown/error never becomes accepted or silently provisional. | State/API/CLI/browser results. |
| T3-02 | Install full with accepted calibration, explicit preview, deferred calibration, and custom hardware; reinstall existing units. | Capture policy matches the selection; preview is opt-in; existing identities/configuration are preserved; custom hardware stays deferred. | Installer tests and configuration hashes. |
| T3-03 | Import a valid matching user bundle on full and minimal with a real reader subscribed. | The active daemon and fresh frame identify the expected bundle/hash and authority; state survives restart. | Activation receipt, frame metadata, status. |
| T3-04 | Dry-run a valid bundle and invalid identity, camera, profile, geometry, hash, missing-file, and path/symlink fixtures. | Valid dry-run makes no persistent/runtime change; incompatible or unsafe input is rejected before activation with a specific reason. | Validation results and before/after hashes. |
| T3-05 | Test trusted, unknown-key, invalid-signature, and unsigned factory bundles, plus valid user bundles. | Factory authority requires the existing factory trust rules; no invalid bundle is accepted or silently relabeled as user validated. | Signature/namespace results. |
| T3-06 | Interrupt activation at durable transaction boundaries; run concurrent import/rollback attempts. | Locking prevents mixed state; restart/recovery loads the last committed valid selection and records a clear failure or pending result. | Fault-injection log and recovered selections. |
| T3-07 | Force daemon startup failure or expected-hash mismatch after staging; invoke rollback after a successful import. | Failed activation restores the previous selection; explicit rollback loads that selection and its matching frame identity when hardware is available. | Transaction receipts and frame hashes. |
| T3-08 | Import/activate while capture is idle, then connect a reader; separately disconnect a camera. | Demand-driven idleness is explained, fresh output verifies activation on request, and camera failure remains distinct from calibration status. | Status transitions and stream trace. |
| T3-09 | Upgrade/reinstall existing full and minimal fixtures and inspect minimal package dependencies/payload. | Existing CLI/bundle formats work; configuration and identity persist; minimal adds no web, SDK, Wi-Fi onboarding, solver, NumPy, or Python OpenCV. | Compatibility and package-isolation report. |
| T3-10 | Review calibration wording and perform a newcomer calibration-readiness task. | The user identifies preview versus accepted calibration and its next action; no label claims independent metric accuracy without the corresponding evidence. | Copy review and observed task result. |

### Final result

Users understand calibration readiness and can import a compatible accepted bundle without editing systemd files. Activation is recoverable, observable, and consistent with the actual output metadata.

## Step 4 Provide executable application integration

### Required behavior

Provide a complete Python quickstart from a fresh virtual environment through verified TLS, credential acquisition, a small depth/validity subscription, and a useful output. Supply a supported public-certificate export procedure and verification against trusted setup material. Administrative tokens remain separate from browser access and must not be written into examples, diagnostic exports, or source control.

Publish runnable examples for a single frame, valid selected-pixel depth, depth display, continuous streaming, recording, and replay. Explain optical-axis depth in metres, validity code semantics, NaN handling, product format/dimensions, calibration authority, and device/run/capture identity. NumPy/display dependencies may be optional example extras; preserve the dependency-light core client.

Include a small distributable real recording with provenance, checksum, license, expected decoded output, and clearly stated provisional/calibrated status. Test-only synthetic fixtures remain clearly labeled. A replay/demo view cannot be mistaken for a live device connection.

SDK metadata must identify its own version and supported runtime compatibility accurately. Preserve current public signatures, accepted recordings, bounded streaming, and explicit gap/error behavior. Add actionable connection/authentication/output-mode errors without silently reconnecting recordings across gaps or weakening TLS verification.

Provide a minimal C example built against the installed public SDK surface and concise public API guidance. Describe ROS 2 as its actual compatibility boundary until a released bridge is independently delivered.

### Deliverables

- D4-01: Python setup guide, certificate/token bootstrap procedure, and version compatibility table.
- D4-02: Executable Python examples, real replay fixture, sample outputs, and supported client environment matrix.
- D4-03: Public C example/API entry guide, actionable SDK errors, and integration status labels.

### Acceptance tests

| ID | Setup and procedure | Required result | Evidence |
| --- | --- | --- | --- |
| T4-01 | Follow the quickstart in clean Linux and Windows client environments using Python 3.11 and one newer recorded supported version. | The documented installation/import and first frame succeed with only declared dependencies and trusted device access. | Environment manifests and transcripts. |
| T4-02 | Connect to real TLS fixtures using the correct certificate, unknown certificate, wrong hostname, and changed device fingerprint. | Valid identity succeeds; untrusted/mismatched identity fails with useful guidance; no example bypasses verification. | TLS integration results. |
| T4-03 | Try valid, invalid, absent, and expired SDK credentials; compare automatic browser connection. | Authentication failures explain the correct SDK recovery; browser instructions remain accurate; errors and logs contain no token. | Auth/session integration and redaction checks. |
| T4-04 | Run every Python example against the matching device and replay fixture. Include valid and invalid pixels and product format variants. | Examples handle units, masks, NaNs, dimensions, and authority correctly; recorded expected results match within declared tolerances. | Example outputs and decoded fixture checks. |
| T4-05 | Record a finite stream; inject disconnect, timeout, early peer close, and interruption. Replay the completed portion. | Finite shortfall fails clearly; complete records remain readable; no silent gap-filling or reconnection changes the recording contract. | Exit codes, recording/replay report. |
| T4-06 | Request disabled raw output, disabled monitoring output, and a subscription exceeding the available link budget. | The error names the cause and supported action; the bounded client does not hang or accumulate an unbounded queue. | Endpoint and client traces. |
| T4-07 | Compile/run the C example against installed headers/libraries; read public API/status instructions. | No private headers or repository-relative build paths are required; ROS 2 maturity is explicit. | Compiler transcript and sample output. |
| T4-08 | Exercise documented previous-compatible SDK calls and older supported recordings against the candidate. | Existing signatures, frame interpretation, bounded reads, and explicit close/error behavior remain compatible; supported version combinations are recorded. | SDK/recording compatibility results. |

### Final result

A developer can connect securely, interpret one valid depth sample, stream or record selected products, and replay data using the published examples without reverse-engineering the repository.

## Step 5 Make diagnostics and recovery actionable

### Required behavior

Extend existing platform preflight and health reporting into one understandable diagnostic report. The report covers supported OS/kernel, camera enumeration, trigger readiness, services, calibration, output modes, current versions, and network/output budget where available. Minimal reports omitted appliance features as intentionally unavailable.

Each check returns a stable identifier, status, reason, and next action. Distinguish fail, warning, and unknown. Checking status is read-only; repairs remain explicitly invoked through existing supported controls. An idle matcher, disabled optional encoder, or pending calibration must not produce a misleading generic disconnected diagnosis.

Provide a downloadable/exportable credential-free support report using allowlisted status/configuration/version fields and curated diagnostic events. Exclude arbitrary raw logs by default. Never include administrative tokens, passwords, cookies, private keys, private setup material, or temporary network-confirmation links. Do not upload reports automatically.

Replace the Software update placeholder with installed-version information, the approved supported upgrade procedure, preservation guarantees that have been tested, and actionable failure/recovery guidance. A new browser updater is not required. Show downgrade/rollback commands only for explicitly tested supported package transitions; preserve the existing rejection of implicit downgrades and mixed cohorts.

Document the current browser access policy, SDK credentials, and trusted-network expectations consistently. This work preserves automatic browser access; adding a new access-control product flow is separate scope.

### Deliverables

- D5-01: Unified human/JSON diagnostics, stable problem codes, and recovery actions in CLI/console.
- D5-02: Allowlisted support export with schema and redaction tests.
- D5-03: Useful Software update page and tested full/minimal upgrade/recovery instructions.

### Acceptance tests

| ID | Setup and procedure | Required result | Evidence |
| --- | --- | --- | --- |
| T5-01 | Inject missing camera, unsupported kernel, unavailable trigger, failed service, missing calibration, and unavailable network in isolated fixtures. | Each condition gets the correct check/reason/action; unavailable data is unknown rather than pass. | Diagnostic fixture results. |
| T5-02 | Check healthy idle capture, image-only consumers, disabled optional encoder, and minimal installation. | Intentional inactivity/omissions are explained without false faults or suggestions to use absent features. | CLI/console snapshots. |
| T5-03 | Run diagnostics and export on a configured device while hashing persistent configuration, identities, network state, and active selection. | Read-only operations leave persistent settings and active service policy unchanged. | Before/after state report. |
| T5-04 | Seed all known secret classes into diagnostic inputs and failure paths; export through CLI/browser and inspect errors. | Secret values and private fields are absent; supported allowlisted fields remain usable; output matches the declared schema. | Redaction tests and inspected sample export. |
| T5-05 | Reinstall and upgrade an explicitly supported previous signed cohort to the candidate on full and minimal. | Identity, calibration, user settings, network choice, and recordings persist; dpkg audit is clean and fresh output returns. | Lifecycle transcript and preservation hashes. |
| T5-06 | Select tampered signatures, corrupted assets, mixed cohorts, full/minimal conflicts, and an implicit downgrade. | The operation rejects the unsupported transition before replacing the working Nuvo cohort; actionable guidance is shown. | Rejection results and protected-file hashes. |
| T5-07 | Interrupt download and package/service work at supported recovery points; follow the documented retry procedure. | Partial downloads are not accepted; retry preserves unit state and restores a working supported installation or names the exact unresolved OS/package repair. | Recovery transcript and final audit. |
| T5-08 | Open Software update on a device with/without reachable release information and follow its terminal procedure. | Installed version remains accurate, unverified availability is labeled, commands are pinned, and the page never implies an implemented browser updater or untested rollback. | Browser walkthrough and upgrade report. |

### Final result

Users can identify the cause of a failed setup, collect a shareable report, and follow a tested maintenance path while preserving device identity and calibration.

## Step 6 Make development reproducible

### Required behavior

Provide tested dependency-install instructions for a named supported Linux distribution and WSL, plus a reproducible host development container or bootstrap environment. Record exact OS/toolchain dependencies. The minimal-host and development-full presets must work from a fresh documented environment without undeclared manual repair.

Resolve the CMake minimum-version mismatch: preset schema version 6 needs CMake 3.25 or later. Either require 3.25 consistently or lower the schema to the actually used compatible features and verify the declared minimum. Reuse existing Debian build dependencies where appropriate, without installing Pi camera backends into generic host workflows.

Provide a deterministic host fixture for gateway/browser/SDK work using the real gateway protocol and fixture frame source. Clearly label it as test/replay data. A host environment does not qualify Pi capture, electrical behavior, real systemd lifecycle, or physical networking.

### Deliverables

- D6-01: Tested host bootstrap/development environment and explicit prerequisite versions.
- D6-02: Aligned CMake requirements and documented minimal/full build/test commands.
- D6-03: Deterministic gateway/frame fixture, contributor instructions, and environment manifests.

### Acceptance tests

| ID | Setup and procedure | Required result | Evidence |
| --- | --- | --- | --- |
| T6-01 | Start a clean supported Linux environment, run only the documented minimal bootstrap and build/test commands. | Configure, build, and every applicable registered test pass without hidden local dependencies. | Provisioning transcript and CTest output. |
| T6-02 | Repeat using the full host environment, including Boost.System, OpenSSL, OpenCV, yaml-cpp, and FFmpeg dependencies. | development-full configures/builds/tests successfully; optional omissions and skips are explicit. | Dependency manifest and full test report. |
| T6-03 | Run the documented minimum CMake and one newer supported version; try a version below minimum. | Supported versions parse/build correctly; unsupported versions fail with a clear requirement. | Toolchain matrix results. |
| T6-04 | Follow supported WSL instructions from a clean documented distribution. | Host builds and fixtures work; native Windows/client guidance and physical Pi tasks are clearly distinguished. | WSL environment and test transcript. |
| T6-05 | Launch the deterministic gateway/frame fixture, then run browser and SDK integration tests. | The actual wire/API contracts are exercised with reproducible identities and labeled fixture output. | Fixture manifest and integration results. |
| T6-06 | Rebuild the environment from declared inputs on another clean runner. | Dependency resolution is reproducible to the declared version policy; no credentials, private datasets, or developer-local paths are required. | Second-run manifest and results. |

### Final result

A new contributor can build and test the host components using one documented environment, while understanding which behavior still requires a real Pi.

## Step 7 Verify supported workflows continuously

### Required behavior

Add required host configure/build/CTest, Python/SDK, and rendered-browser checks for relevant pull requests and pushes. Use path-aware lightweight checks for documentation. Preserve manual ARM64 package/signing workflows and the repository rule that documentation-only work does not start package builds or signing.

Browser tests must use the real gateway and deterministic frame fixture where possible. Cover first connection, readiness, unavailable devices, selection of output products, validation, configuration revision conflicts, reconnect, calibration transitions, and diagnostics. Retain existing static/JavaScript tests as complementary coverage.

Record a frozen candidate manifest before physical acceptance: source commit, package/bundle hashes, signing identity when applicable, OS image/hash, kernel, dependency versions, actual Pi SKU/RAM, camera IDs, calibration hash/authority, controls, network, and test timestamps. Hosted ARM64 builds and memory-limited large boards cannot substitute for a physical advertised smaller SKU.

Test fresh supported installations, upgrade/reinstall, physical reboot/power cycle, Ethernet, protected setup Wi-Fi, failed handoff/recovery, and each retained advertised RAM/profile combination. Reuse one physical run across applicable test IDs when the manifest, conditions, and evidence satisfy all of them.

### Deliverables

- D7-01: Required CI matrix, deterministic rendered-browser suite, and retained artifacts.
- D7-02: Physical acceptance runner/checklists extending existing HIL tools, with preservation and restoration checks.
- D7-03: Supported hardware/client matrix, candidate evidence manifest, and completion report mapping every requirement/test to evidence.

### Acceptance tests

| ID | Setup and procedure | Required result | Evidence |
| --- | --- | --- | --- |
| T7-01 | Exercise the PR workflow with deliberate native, SDK, and UI regressions in disposable fixtures. | The relevant required job fails; logs identify the failure; skipped/missing required jobs cannot look green. | Workflow runs and retained logs. |
| T7-02 | Change only user documentation, then change runtime/UI code in separate trials. | Docs run lightweight consistency/link checks only; code runs relevant host/browser coverage. ARM64 packaging/signing remains deliberately dispatched. | Workflow routing results. |
| T7-03 | Run rendered-browser scenarios for connect, calibration states, stale/unavailable health, form validation, revision conflicts, output selection, restart/reconnect, and diagnostics. | Correct actions/messages appear, pending edits are protected, and no unhandled error blocks the task; screenshots/traces are retained on failure. | Browser test report and traces. |
| T7-04 | Install each profile from frozen candidate artifacts on a newly flashed supported image using physical cameras. | Signed-path verification succeeds where applicable; fresh output and correct authority return after required reboot; exact environment/artifact identity is retained. | Physical install, frame, and package reports. |
| T7-05 | Perform ten graceful reboot cycles and five physical power-cycle tests per profile; include a controlled interrupted activation/recovery case on a recoverable test device. | Identity/configuration/committed calibration survive; services recover according to policy; fresh frames return with a new run identity; no mixed committed state. | Cycle records, state hashes, recovery evidence. |
| T7-06 | Exercise Ethernet and twenty setup Wi-Fi handoffs covering success, wrong credentials, missed confirmation, reboot during handoff, and outage after confirmed setup. | Confirmed setup persists; failed/unconfirmed setup returns to the protected hotspot with the same secret; an ordinary confirmed-network outage does not reopen it. | Network transition/status records. |
| T7-07 | Run the retained advertised workload on every claimed physical Pi RAM SKU/profile combination. | Each retained claim has physical evidence; unsupported combinations are removed or clearly marked unqualified before completion. | Physical SKU matrix, memory/thermal/frame reports. |
| T7-08 | Assemble the final candidate report and compare every artifact/hash with the tested manifest. | Every required test has valid candidate evidence; failure, blocked, skipped, or not-run cases remain visible and prevent the affected completion gate. | Signed/reviewed completion report and evidence index. |

### Final result

Future changes receive meaningful automatic regression coverage, and self-service readiness is backed by exact-artifact tests on the physical configurations users are told to use.

## Performance and resource acceptance

### Required behavior

Keep the existing measured two-thread CPU-rectification baseline unless new controlled evidence supports another default. Preserve demand-driven processing and bounded frame ownership. An image-only subscriber must not force stereo matching, and slow clients must not create unbounded work or queues.

Offer clearly described task-based selections for local depth/validity, a conservative network depth stream, and monitoring. Choose only required products. Explain bandwidth, achievable cadence, quality/near-range tradeoffs, and whether a stream is exact or best effort. A recommendation must not silently change an exact delivery request or quality controls.

Show capture target, observed image/depth completion rate, client receive rate, freshness, and compute versus end-to-end latency distinctly. Inverse matcher duration and image-only publication counts cannot be presented as completed depth throughput. All provisional/candidate labels remain until their applicable qualification gates pass.

Use identical physical hardware, OS/dependencies, camera/scene conditions, controls, calibration, consumers, and network for baseline/candidate comparisons. Record three ten-minute measured runs after a five-minute warm-up for core comparisons; perform a separate 60-minute soak per retained shipping-profile workload. Capture distributions, resource use, errors, and unique frame identities.

Proposed regression budgets for this project are candidate depth rate at least 95% of baseline, end-to-end p95 latency at most 110% of baseline, and steady service RSS at most 110% of baseline. These are acceptance budgets, not measured results or universal product promises. If differences fall within run variability, extend measurement rather than declare a pass from noise. Investigate any new trigger, requeue, scheduler, protocol, or identity error. Preserve old absolute product thresholds wherever stronger.

### Deliverables

- DP-01: Task-based output recommendations, honest performance labels, and bandwidth guidance.
- DP-02: Reproducible baseline/candidate benchmark and thermal/resource soak reports.
- DP-03: Per-profile measured results linked to exact controls, artifacts, calibration, network, and physical SKU.

### Acceptance tests

| ID | Setup and procedure | Required result | Evidence |
| --- | --- | --- | --- |
| TP-01 | Subscribe to image-only, depth/validity, preview-only, combined, and zero-consumer cases. | Only demanded stages run; idle matcher/encoder work remains idle as appropriate; selected products share capture identity. | Stage counters, CPU and frame trace. |
| TP-02 | Measure full/minimal core depth, mixed preview, and multiple bounded clients against the frozen baseline. | Regression budgets pass without changing matcher quality, calibration, geometry, or consumer demands to hide cost. | Three-run distributions and configuration hashes. |
| TP-03 | Sweep admitted and excessive product/cadence requests across measured Ethernet/Wi-Fi budgets. | Requests are admitted/rejected according to policy; exact requests are not silently degraded; best-effort gaps/cadence are explained accurately. | Admission decisions and measured bytes/rates. |
| TP-04 | Run slow/stalled clients, disconnect/reconnect, and retained-frame consumers through the 60-minute soak. | Queues/ownership remain bounded; retained data stays immutable; no sustained RSS growth, swapping, crash, or new core error occurs. | RSS/CPU/thermal series and frame/lease results. |
| TP-05 | Compare displayed capture, depth completion, receive rate, freshness, and latency with independent observations. | Labels correspond to their actual measurement boundaries and tolerances; no stale or image-only output inflates depth performance. | Independent trace and UI/API comparisons. |
| TP-06 | Process a fixed recorded stereo fixture with identical algorithms/controls before and after the changes. | Disparity/depth/validity and calibration interpretation meet the frozen equivalence tolerances; user-experience work introduces no hidden quality change. | Golden comparison, dependency versions, tolerances. |
| TP-07 | Review every retained rate/accuracy/range/RAM claim against candidate evidence; independently measure accuracy if new metric claims are proposed. | Claims stay within measured scope. New accuracy claims require predeclared distance/scene/coverage, bias, and RMSE limits and independent ground truth; otherwise existing provisional labels remain. | Claim-to-evidence table and applicable accuracy report. |

### Final result

The simpler defaults preserve measured quality and throughput within declared budgets, respect link/resource limits, and communicate what the device actually produces.

## Regression requirements across all steps

These checks protect existing supported behavior in addition to the feature-specific tests. Run only applicable suites in each change, then the complete required candidate matrix before final acceptance. Reuse evidence instead of repeatedly running identical checks without a new change or unresolved failure.

| ID | Procedure | Required result | Evidence |
| --- | --- | --- | --- |
| TR-01 | Run all applicable existing and new native/Python/JavaScript tests for minimal-host, development-full, and both ARM64 package profiles. | All required registered tests pass. Any removed test has a reviewed replacement/rationale; optional skips name the missing capability and cannot satisfy a required gate. | CTest/test-runner inventories and results. |
| TR-02 | Exercise local IPC, C ABI, HTTPS snapshot, WebSocket, NUV/NVE decoding, and NVR recording/replay with current and documented compatible clients. | Public structures/formats and supported old recordings remain compatible; malformed/bounded input checks continue to reject invalid data. | Protocol/ABI/recording compatibility report. |
| TR-03 | Inspect associated frames across concurrency, overload, reconnect, and daemon restart. | Products share the expected device/run/capture identity; streamed frames have no unintended duplicate or reversed identity within a run; whole-frame gaps are reported where best effort permits them. Repeated latest snapshots retain their existing allowed semantics. | Identity/immutability trace. |
| TR-04 | Apply valid/invalid exposure, stereo, and encoding settings, including simultaneous edits and formatting-only rewrites. | Revision preconditions protect newer edits; atomic writes retain the last valid config; requested/saved/applied are distinct and frame epochs remain correct. | API/CLI receipts and frame metadata. |
| TR-05 | Exercise TLS, origin/CSRF/session, permissions, calibration trust, and command allowlists around changed paths. | Existing protections remain effective; the gateway gains no general root/camera access; diagnostic and error paths expose no secrets. | Boundary and permission tests. |
| TR-06 | Install/reinstall/upgrade/remove the packages in disposable supported environments and inspect artifacts. | Package metadata/cohorts are consistent, dependency isolation holds, identity/calibration/recordings are preserved by supported operations, and dpkg audit is clean. | Lifecycle and payload reports. |
| TR-07 | Test protected hotspot, new-path confirmation/rollback, confirmed outages, and existing SSH/network preservation. | Existing network policy and recovery behavior remain compatible; no repair/check silently replaces network configuration. | Network state and preservation report. |
| TR-08 | Exercise interrupt/crash/low-space/malformed-state cases in durable configuration, calibration, and recording operations. | The last committed valid state survives, incomplete output is handled by the existing contract, and failure is explicit without false success. | Fault-injection/recovery report. |
| TR-09 | Run browser tasks at desktop and 390-pixel mobile widths, keyboard-only navigation, and light/dark themes in recorded supported browser versions. | Required controls/actions remain reachable, focus and error messages are usable, text is readable, and no blocker/uncaught error prevents completion. | Browser screenshots and accessibility task checklist. |
| TR-10 | Verify repository/export hygiene, workflow permissions, release routing, and protected test-device restoration. | No private/generated junk enters source bundles; docs do not trigger signing/builds; test changes restore protected host state; public release actions follow repository policy. | Hygiene/workflow results and restoration hashes. |

## Verification execution

These existing commands are entry points for host verification from the source checkout in the documented Linux/WSL environment. They do not exercise physical cameras:

```bash
cmake --preset minimal-host
cmake --build --preset minimal-host -j 2
ctest --preset minimal-host --output-on-failure

cmake --preset development-full
cmake --build --preset development-full -j 2
ctest --preset development-full --output-on-failure
```

Use focused existing checks while implementing the corresponding feature; avoid repeating an identical passing check when it was already covered by the unchanged CTest run:

```bash
bash -n install-nuvo-depth.sh
bash -n install-nuvo-depth-minimal.sh
python3 tests/install_provisioning_test.py
python3 tests/install_release_test.py
python3 tests/minimal_install_test.py
python3 tests/minimal_cli_test.py
python3 tests/nuvoctl_test.py
python3 tests/system_agent_test.py
python3 tests/python_sdk_test.py
python3 tests/calibration_activation_test.py
python3 tests/calibration_session_test.py
python3 tests/web_ui_static_test.py
```

These commands are partial coverage, not a runner for all 69 acceptance cases. Each implementation change must map its cases to an existing test, a new meaningful automated test, or a recorded manual/physical procedure. D7-03 must include the exact invocation, fixtures, supported environments/cohorts, timeout, expected exit status, and resulting evidence for every automated case; manual cases include reproducible operator instructions. Rendered-browser tests, new calibration commands, diagnostic export, and new consistency checks are proposed features and need their own documented commands when implemented.

Run physical/HIL and package-lifecycle procedures only on the designated recoverable test environment with the required device access, isolation, backup, and restoration steps. Existing HIL scripts are starting points and must be checked against the chosen full/minimal environment; an isolated Debian userspace result must not be recorded as a fresh supported Pi OS installation or physical kernel reboot. Test reports must distinguish unsigned Actions package testing, controlled signed-fixture verification, and an approved public signed installer run.

## Evidence and result format

Each acceptance case must record test ID, applicable requirement/step, source commit, candidate artifact hashes, environment/cohort reference, procedure version, start/end times with timezone, observed values, expected criteria, status, artifact paths/URLs, and reviewer. Performance cases include raw samples and the baseline cohort; physical cases include actual hardware identity.

Allowed statuses are `not_run`, `pass`, `fail`, `blocked`, and `not_applicable`. A blocked case names the missing hardware/access/input. Not applicable requires a reason tied to the published support scope and a reviewer; it cannot replace required fresh-Pi, advertised-SKU, core regression, or newcomer evidence. A flaky failure stays failed until diagnosed and fixed, then rerun with all attempts retained.

Keep a requirement-to-test-to-evidence index. Missing evidence, mismatched hashes, unfinished wiki updates, or a test run from an older candidate prevent the affected gate from passing. Test thresholds must be frozen before execution; do not loosen them after seeing results to create a pass.

## Final delivery package

The completed implementation must include these artifacts:

1. Reviewable source changes and an updated published user guide matching the implemented behavior.
2. The D1 through D7 and DP deliverables listed above, with links and owners in the completion report.
3. Native/Python/SDK/browser test reports, environment manifests, and CI run links for the exact candidate.
4. Full and minimal Actions package artifacts, package inventories/checksums, dependency/lifecycle reports, and matching source/build records.
5. Physical installation, network, reboot/power-cycle, calibration recovery, RAM-SKU, and performance/thermal reports.
6. Five newcomer walkthrough records, with failed sessions and fixes retained.
7. A filled acceptance results record and a concise completion report listing passed gates, supported configurations, measured limits, and unresolved issues.

Routine implementation/testing uses Actions artifacts. ARM64 test package builds remain manual. Do not create tags, draft release candidates, publish releases, mark latest, or run signing merely to complete this plan. Those actions follow the repository's explicit release-authorization policy. A documentation-only change does not dispatch ARM64 packaging or signing. A final public signed installer qualification requires a separately authorized release process; the proposed work can first be verified with appropriately labeled candidate artifacts and controlled signed fixtures.

## Completion checklist

- [ ] Every numbered step and performance workstream has its deliverables linked.
- [ ] Current documentation and published wiki agree with implemented behavior and the approved public installation reference.
- [ ] Full and minimal installation paths produce their documented first usable output.
- [ ] Readiness is consistent, unknown/error stays distinct, and preview never implies verified metric accuracy.
- [ ] Calibration import, dry-run, activation verification, rollback, and interruption recovery pass on both profiles.
- [ ] Python and C examples run in the declared supported environments; recording/replay and previous supported contracts pass.
- [ ] Diagnostics are actionable/read-only and exports pass secret-exclusion checks.
- [ ] Supported reinstall/upgrade paths preserve identity, calibration, settings, network policy, and recordings.
- [ ] Clean developer environments reproduce applicable builds and tests.
- [ ] Required CI runs on relevant changes and cannot hide skipped/missing required checks.
- [ ] Fresh physical Pi, network, reboot/power-cycle, and every retained advertised SKU gate pass.
- [ ] Controlled performance comparisons and soak runs meet frozen budgets without weakening quality.
- [ ] Newcomer tasks pass without maintainer assistance and meet the specified active-time targets.
- [ ] The exact candidate evidence index is complete and all failures/blockers affecting advertised workflows are resolved.
- [ ] Completion claims distinguish software implementation, self-service beta acceptance, and separate production qualification.

## Existing baseline and reference evidence

The repository review successfully configured/built minimal-host in Linux/WSL and ran 43 CTest entries with zero failures. This is a baseline result, not evidence that the new work is implemented. The reviewed development-full configure stopped at the environment's missing Boost.System component, which the reproducible-development work must address. The acceptance cases in this specification have not been executed against an implementation candidate.

Primary source references:

- [Repository and current entry point](https://github.com/arputhamips/nuvo-depth/tree/6537e2cafbd154a70c0d044097c4d3e1f52803f1).
- [Installer reference and existing setup behavior](https://github.com/arputhamips/nuvo-depth/blob/6537e2cafbd154a70c0d044097c4d3e1f52803f1/docs/installer-reference.md).
- [Minimal profile and calibration-selection limitation](https://github.com/arputhamips/nuvo-depth/blob/6537e2cafbd154a70c0d044097c4d3e1f52803f1/docs/minimal-install.md).
- [Console structure and current copy](https://github.com/arputhamips/nuvo-depth/blob/6537e2cafbd154a70c0d044097c4d3e1f52803f1/web/index.html).
- [Authoritative matcher profile catalog](https://github.com/arputhamips/nuvo-depth/blob/6537e2cafbd154a70c0d044097c4d3e1f52803f1/web/depth-profiles.json).
- [Python SDK contracts and examples](https://github.com/arputhamips/nuvo-depth/blob/6537e2cafbd154a70c0d044097c4d3e1f52803f1/sdk/python/README.md).
- [Build instructions](https://github.com/arputhamips/nuvo-depth/blob/6537e2cafbd154a70c0d044097c4d3e1f52803f1/docs/build-and-test.md) and [workflows](https://github.com/arputhamips/nuvo-depth/tree/6537e2cafbd154a70c0d044097c4d3e1f52803f1/.github/workflows).
- [Beta 6 package and physical-test scope](https://github.com/arputhamips/nuvo-depth/blob/6537e2cafbd154a70c0d044097c4d3e1f52803f1/docs/release-qualification-beta6-20261021.md).
- [Performance settings and recorded measurements](https://github.com/arputhamips/nuvo-depth/blob/6537e2cafbd154a70c0d044097c4d3e1f52803f1/docs/performance-profiles.md).
- [Existing physical qualification gates](https://github.com/arputhamips/nuvo-depth/blob/6537e2cafbd154a70c0d044097c4d3e1f52803f1/qualification/pi5-beta.yaml).
- [CMake preset schema compatibility](https://cmake.org/cmake/help/v3.25/manual/cmake-presets.7.html#format).
