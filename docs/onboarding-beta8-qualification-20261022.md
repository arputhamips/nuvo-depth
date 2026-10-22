# Beta 8 onboarding qualification — execution in progress

Implementation source: `2b47a791e7be505ad2ed9871dc07a05a577fd35f`.
Public preview remains beta 6. Beta 8 has not been released or publicly approved.
This report will retain failed attempts and distinguish software, cleaned-system,
fresh-image, physical and human evidence. Neither acceptance gate is complete.

## Implemented and locally checked

The source adds transactional installer import, typed readiness/error handling,
fresh-output receipts, pre-upgrade minimal policy migration, allowlisted CLI/
console diagnostics, useful update instructions, runnable Python/C examples,
an older real recording fixture, clean host bootstrap/CI, a real gateway browser
fixture and the complete 69-case procedure/evidence index.

The existing WSL environment passed 67/67 registered development-full CTest
entries, including real gateway integration. Ten rendered first-use fixture
checks and eight actual-gateway browser journeys passed. Fresh Windows Python
3.11/3.13 environments installed SDK beta 8; TLS checks passed on both, and
SDK/first-use suites passed on 3.13. Clean hosted matrices are still running;
the WSL result does not qualify a newly provisioned WSL distribution.

Wiki revision `0fd4015207e72411f0ccb65bb3c1ebfdd831d4ac` built and validated:
33 HTML pages, 168 assets, release/setup/first-use checks and internal references.
Its production deployment workflow succeeded. Live-page verification is pending.

## Physical cleaned-system trial

The operator authorized deletion without a new backup. `/home/nuvo` contents
and application/development packages were removed from the existing OS, retaining
OS networking, SSH, camera support and protected identity/per-unit calibration
outside home. This is **cleaned existing Raspberry Pi OS**, not a fresh-flash or
factory-state qualification. The actual board is Pi 5 Rev 1.1, approximately
16 GB physical RAM, kernel `6.12.75+rpt-rpi-2712`.

The public GitHub rolling minimal bootstrap selected and verified signed beta 6.
Dry-run and installation passed. A real kernel reboot was performed. Documented
float-depth capture created depth, validity and metadata files; NUV capture and
three-frame stream also worked. Output remained authority 0 provisional. The
beta 6 seed predates the restored beta 5 default in the beta 8 candidate.
Ethernet is connected for recoverable Wi-Fi testing. Candidate upgrade/lifecycle
tests remain pending the exact ARM64 artifacts.

## Issues encountered and their handling

| Finding | Handling / remaining evidence |
| --- | --- |
| Earlier minimal capture gating could disable implicit preview on upgrade, especially with a stopped daemon | Preinst snapshots the old effective policy before replacing the unit; stopped/no-health fixture tests pass. Actual beta 6 -> beta 8 upgrade pending. |
| Full setup imported a bundle before the shared verified activation transaction | Supplied bundles are validated first and activated through the common transaction after service configuration; startup failure restores prior policy. Reboot-required import stays pending. |
| Malformed status/journal values could raise type errors | Typed validation reports unavailable/actionable errors; invalid state never grants authority. |
| Diagnostics/update UI lacked a supported safe export and maintenance path | Added fixed read-only operation and schema allowlist, secret-class tests and current version/recovery instructions. |
| Host prerequisites were incomplete and CMake minimum disagreed with presets | Named Debian/Ubuntu bootstrap, Boost/full dependencies, CMake 3.25 alignment and clean CI/minimum-version jobs added. |
| Wiki recommended four threads and unqualified package rollback; README claimed small RAM suitability | Corrected to two-thread measured baseline, same-cohort recovery and explicitly unqualified smaller physical SKUs. Historical reports remain unchanged. |
| Windows TLS fixture's localhost IPv6 attempt consumed its 2-second budget | Fixture uses direct IPv4 with matching SAN; hostname rejection still covered. Corrected tests pass on 3.11/3.13. |
| Purging Python YAML would remove OS NetworkManager/cloud-init | Dependency simulation identified this before removal; retained OS dependency and working networking. |
| Preserved original per-unit bundle UUID differs from the current host UUID | Neither identity nor manifest was rewritten. Accepted import on this unit requires operator reconciliation of original identity history; preview tests remain honest authority 0. |

## Evidence and remaining gates

Manual ARM64 artifact runs:
[minimal](https://github.com/arputhamips/nuvo-depth/actions/runs/39870888711),
[full](https://github.com/arputhamips/nuvo-depth/actions/runs/39870891131).
Required clean host/browser/client checks:
[candidate run](https://github.com/arputhamips/nuvo-depth/actions/runs/39870868011).
Intentional native/SDK/browser regressions are separate disposable workflow runs;
their expected failures must identify the injected cause before CI is qualified.

The local execution directory `output/spec-implementation-20261022` retains
canonical Pi command results, the append-only `pi-commands.jsonl` command/transfer
audit, package cleanup transcript, protected-file hashes, real frame metadata,
host CTest and browser traces. Credentials are excluded from the audit. This
private/generated evidence stays outside source bundles.

The procedure index, hardware manifest, frozen performance budgets and newcomer
forms are in [qualification/onboarding](../qualification/onboarding/README.md).
Freshly flashed storage, physical power interruption, independent participant
walkthroughs and any unavailable physical RAM SKU must not be replaced by agent
simulation, SSH reboot, a memory cap or an isolated userspace test. Missing cases
remain visible and prevent self-service acceptance. No new metric-accuracy,
production or universal rate/RAM claim is made.
