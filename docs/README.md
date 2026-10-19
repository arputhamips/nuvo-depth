# Nuvo Depth documentation

This is the user and operator documentation index for the Raspberry Pi 5
appliance. Start with the user guide; use the engineering references only when
changing the image pipeline or qualifying hardware.

## Use the camera

- [User guide](user-guide.md) — first connection, normal workflow, exposure,
  stereo matching, streaming, and product delivery.
- [Operations and troubleshooting](operations-troubleshooting.md) — service
  checks, recovery, flicker diagnosis, and safe rollback.
- [Pi 5 release-readiness report](pi5-release-readiness-20260914.md) — current
  verified behavior, performance, and the remaining release gates.
- [Python SDK](../sdk/python/README.md) — authenticated raw and monitoring
  clients.

## Configure and deploy

- [Terminal/factory installer](install.md) — prompted setup, current-network or
  protected-hotspot access, manufacturing defaults and signed GitHub assets.
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

- [Debian archive and APT publication roadmap](debian-archive-roadmap.md) —
  current blockers, the official Debian sponsorship path, and the immediate
  signed vendor-repository route.

The checked-in calibration is provisional. The software is usable for
engineering disparity and pipeline work, but metric depth is not a calibrated
product until the next per-unit calibration and acceptance run passes.
