# Nuvo Depth documentation

The [Nuvo Hardware website](https://nuvohardware.com/) introduces Vision One.
The [user wiki](https://docs.nuvohardware.com/vision-one/) is the main guide for
setup and operation. This directory holds versioned technical references,
specifications, and qualification evidence.

## Use the camera

- [Diagnostics and upgrades](diagnostics-and-upgrades.md) — read-only reports,
  preview-preserving upgrades and supported recovery.
- [Runnable Python and C examples](sdk-examples.md) — verified TLS, display,
  recording/replay and installed public C ABI.
- [Onboarding acceptance](../qualification/onboarding/README.md) — all 69 cases,
  frozen budgets, physical procedures, newcomer forms and fail-closed gates.

- [First-use journey validation](first-use-validation-20261022.md) — rolling
  signed install selection, first-depth/sample checks, browser fixtures and limits.

- [Set up a bundled unit](https://docs.nuvohardware.com/vision-one/install/bundled-pi).
- [Install on your own Pi or SD card](https://docs.nuvohardware.com/vision-one/install/byo-pi).
- [First browser connection](https://docs.nuvohardware.com/vision-one/guide/getting-started).
- [User guide](user-guide.md) — first connection, normal workflow, exposure,
  stereo matching, streaming, and product delivery.
- [Operations and troubleshooting](operations-troubleshooting.md) — service
  checks, recovery, flicker diagnosis, and safe rollback.
- [Beta 5 signed-cohort qualification](release-qualification-beta5-20261021.md) —
  signed upgrade/public installer rerun, direct-session API, Pi-local performance and limits.
- [Earlier signed release and setup qualification](release-qualification-20261019.md) —
  prior cohorts' installation, network, ownership, and bounded runtime evidence.
- [Independent image/depth Pi retest](performance-independent-processing-20261020.md) —
  mono 10 Hz/depth 1 Hz with `hh`, encoded cold starts, and Wi-Fi delivery limits.
- [Cleaned runtime Pi retest](performance-cleanup-pi-20261021.md) — preview and
  depth both requested at 10 Hz, depth-only baseline, freshness and restoration.
- [Pi 5 performance comparison](performance-comparison-20261019.md) — repeated
  mono 10 Hz/depth 1 Hz measurements, Wi-Fi/API and browser results, and limits.
- [Pi 5 release-readiness report](pi5-release-readiness-20260914.md) — earlier
  performance evidence and remaining physical release gates.
- [Python SDK](../sdk/python/README.md) — authenticated raw and monitoring
  clients.

## Configure and deploy

- [Install Nuvo Depth](install.md) — supported Pi OS and installation command.
- [Minimal raw-depth installation](minimal-install.md) — public preview and Actions test package,
  SSH setup, terminal configuration and raw local output.
- [Minimal package Pi acceptance](minimal-package-qualification-20261021.md) —
  exact artifact, physical camera tests and full-appliance restoration.
- [Installer reference](installer-reference.md) — terminal options, network setup and recovery.
- [Manufacturing provisioning](factory-provisioning.md) — internal unit preparation.
- [Production deployment](../deployment/README.md)
- [Verified Pi 5 full-stack deployment record](pi5-full-stack-deployment-20260915.md) —
  exact clean-board install steps, package hashes, trigger/API/reboot acceptance,
  measured pipeline performance, and the remaining release boundary.
- [Customer-owned Pi 5 prerequisites](pi5-byo-setup-prerequisites.md) — clean
  board state, physical and OS requirements, trigger preparation, install
  order, qualification gates, and the automation boundary.
- [Pi 5 image-stream encoding](pi5-image-stream-encoding.md)
- [Complete stereo image-pipeline and repository map](image-pipeline-map.md) —
  Mermaid diagrams for capture, matching, delivery, controls, calibration,
  compatibility branches, and source-verified integration gaps.
- [Native runtime, IPC, and gateway](native-runtime-ipc-gateway.md)
- [Pi 5 product geometry](pi5-depth-100mm-profile.md)
- [Build and test](build-and-test.md)

## Product flow specifications

These specifications describe proposed setup behavior; implementation and
qualification status are recorded in each document.

- [Shared setup contract](specs/shared-setup-contract.md) — one provisioning backend,
  prompted/factory inputs, explicit access policy, shared ownership, and cross-flow recovery rules.
- [User flow — Use case 1: assembled Nuvo device](specs/user-flow-use-case-1.md) —
  first connection, protected setup hotspot, pairing, network handoff, and recovery.
- [User flow — Use case 2: user-owned Pi or SD card](specs/user-flow-use-case-2.md) —
  supported base OS image, terminal selections and automated installation, factory defaults, and reboot/handoff.

## Calibrate and qualify

- [Advanced calibration profile production](advanced-calibration.md) — the
  guided browser and batch-manufacturing process for ChArUco capture, robust
  holdout validation, profile matrices, activation, and rollback.
- [Calibration UI and Pi 5 validation](calibration-ui-validation-20260916.md) —
  live browser/device results, package checksums, runtime measurements, and the
  remaining physical-target acceptance step.
- [Hardware-in-loop qualification](pi5-hil-qualification-20260909.md)
- [Hardware qualification handoff](hardware-qualification-handoff-prompt.md)
- [Stereo exposure sweep](stereo-exposure-sweep.md)
- [Stereo exposure evaluation and implementation](stereo-exposure-evaluation-20260916.md) —
  research, native controller decisions, UI/control flow, validation, and the
  remaining electrical flicker gate.
- [Calibration and configuration schema](config-schema.md)

## Package and distribute

- [Release operations](releases.md) — signed GitHub bundles and matching source.
- [Beta 6 full and minimal qualification](release-qualification-beta6-20261021.md) —
  separate signed installers, exact package cohorts, physical-camera checks and limits.
- [Repository audit](repository-audit-20261019.md) — retained file groups,
  generated-file exclusions, and package/export checks.
- [Repository cleanup](repository-cleanup-20261021.md) — retired editor, legacy
  processing stack and demo assets, compatibility boundaries, and validation.
- [Direct browser access and beta 5 preparation](browser-access-beta5-20261021.md) —
  automatic sessions, installer integration, compatibility and source checks.
- [Historical architecture review](history/multi-platform-review.md) — an
  archived review of an earlier system, not current setup instructions.
- [Historical deployment options](architecture-options.md) — early robot and
  accelerator assumptions, superseded by the current native runtime/gateway.
- [Debian archive and APT publication roadmap](debian-archive-roadmap.md) —
  current blockers, the official Debian sponsorship path, and the immediate
  signed vendor-repository route.

The checked-in calibration is provisional. The software is usable for
engineering disparity and pipeline work, but metric depth is not a calibrated
product until the next per-unit calibration and acceptance run passes.
