# Nuvo Depth

<!-- BEGIN APPROVED SETUP -->
Approved public release: **`v0.1.1-beta.8`**. Start with **Balanced**.
Use Raspberry Pi OS Lite 64-bit Trixie (`arm64`), kernel `6.12.75+rpt-rpi-2712`.
The rolling command follows this approved reference; development builds and
draft candidates are never selected automatically. Quality is an optional
profile with greater processing cost.
<!-- END APPROVED SETUP -->

Nuvo Depth is the software that powers **Nuvo Vision One**, NuvoHardware's
stereo vision camera. It captures synchronized images from two cameras,
computes depth, and provides a web console and APIs for robotic applications.

[Website](https://nuvohardware.com/) ·
[User wiki](https://docs.nuvohardware.com/vision-one/) ·
[Pi installation](https://docs.nuvohardware.com/vision-one/install/byo-pi) ·
[Downloads](https://docs.nuvohardware.com/vision-one/releases/)

The current supported platform is a **Raspberry Pi 5 with the Nuvo dual-IMX296
camera head**. This repository contains the device software, SDKs, installer,
and release tooling.

## Build your own or buy Vision One

You can also build your own. The main components are:

- **2 × InnoMaker IMX296 color global-shutter cameras:**
  [Camera product page](https://www.inno-maker.com/product/cam-imx296color/).
- **A Raspberry Pi 5 with active cooling:** RAM suitability depends on the
  workload. Smaller RAM boards remain unqualified for this candidate; a memory
  limit on a larger board does not qualify a physical 1 GB or 2 GB model.
  [Raspberry Pi 5 product page](https://www.raspberrypi.com/products/raspberry-pi-5/).
- **A Dupont Y-splitter** to share the hardware trigger signal between both
  cameras.

**Build guide:** [Build a stereo camera](https://docs.nuvohardware.com/vision-one/hardware/build-stereo-camera).

Or [order Vision One from NuvoHardware](https://nuvohardware.com/).

## What is included

- **Camera and depth runtime:** synchronized capture, image rectification,
  stereo matching, and associated image, disparity, depth, confidence, and
  validity outputs.
- **Web console:** device setup, health, live previews, camera controls, and
  calibration tools, accessed directly through an HTTPS gateway without browser login.
- **Application interfaces:** C and Python SDKs, local IPC, and network delivery
  for integrating camera output into your application.
- **Installation and maintenance:** terminal setup, protected Wi-Fi setup
  hotspot, system services, signed Debian packages, and GitHub release builds.

## Get your first depth frame

Choose one complete journey:

- **[Preinstalled Vision One](https://docs.nuvohardware.com/vision-one/install/bundled-pi):** connect, verify the device certificate, inspect both cameras and save depth.
- **[My own Raspberry Pi](https://docs.nuvohardware.com/vision-one/install/byo-pi):** prepare supported hardware/OS, install, reboot and complete the same first-depth checks.
- **[Application integration](https://docs.nuvohardware.com/vision-one/sdk/python):** verify certificate/token access, consume depth and validity, and save an NVR1 recording.

The [complete walkthrough](https://docs.nuvohardware.com/vision-one/guide/getting-started)
explains calibration status and recovery at each step. For SSH-only local
output, use the [minimal terminal journey](https://docs.nuvohardware.com/vision-one/install/minimal).

Use a Raspberry Pi 5 with the supported dual-IMX296 head, Raspberry Pi OS Lite
64-bit Trixie (`arm64`), kernel `6.12.75+rpt-rpi-2712`, internet and administrator
access. Choose one installation:

```bash
# Full appliance with browser interface
curl -fsSL https://raw.githubusercontent.com/arputhamips/nuvo-depth/main/install-nuvo-depth.sh | sudo bash

# Minimal local raw depth; SSH and an existing network are required
curl -fsSL https://raw.githubusercontent.com/arputhamips/nuvo-depth/main/install-nuvo-depth-minimal.sh | sudo bash
```

These maintained bootstraps resolve the approved public release
with a complete signed bundle for the selected profile, as recorded in `deployment/public-release.json`. Drafts are excluded. The selected exact tag is printed and signatures,
manifest, installer and package cohort are verified before installation.
Use `--release TAG` for an exact version; release-page commands remain pinned.
Updates preserve identity, network state and existing validated per-unit
calibration. Shared seed authority remains provisional.

## Current status

- Public preview **beta 8** provides signed full and minimal bundles, consistent
  calibration readiness, existing-bundle import/dry-run/rollback, diagnostics,
  first-depth guidance and runnable application examples.
- The [beta 8 qualification report](docs/onboarding-beta8-qualification-20261022.md)
  records exact artifacts, actual Pi journeys and remaining limits: 53 cases pass,
  15 remain blocked and one removal/profile-switch case fails. The operator
  stopped the remaining long tests; public preview approval does not establish
  complete self-service, production, electrical or metric qualification.
- Validated per-unit calibration is preserved. Shared beta 5 geometry remains
  provisional; a software upgrade alone does not require recalibration.
- Routine tests use Actions artifacts. New candidates stay draft until explicitly
  approved. Earlier [beta 6 evidence](docs/release-qualification-beta6-20261021.md)
  and [beta 5 evidence](docs/release-qualification-beta5-20261021.md) retain their
  original cohorts and version references.
- [Changelog](CHANGELOG.md), [release process](docs/releases.md) and
  [minimal raw-depth installation](docs/minimal-install.md).

## Documentation

The **[Nuvo Hardware wiki](https://docs.nuvohardware.com/vision-one/)** is the
main user documentation. This repository retains versioned technical references
and test evidence.

| You want to… | Start here |
| --- | --- |
| Set up and use Vision One | [Setup guide](https://docs.nuvohardware.com/vision-one/guide/getting-started) |
| Install on your Pi | [Pi installation guide](https://docs.nuvohardware.com/vision-one/install/byo-pi) |
| Integrate camera output | [SDK guide](https://docs.nuvohardware.com/vision-one/sdk/) and [Python SDK reference](sdk/python/README.md) |
| Configure installer options and recovery | [Installer reference](docs/installer-reference.md) |
| Understand the runtime and repository | [Image pipeline and repository map](docs/image-pipeline-map.md) |
| Build or publish packages | [Build and test](docs/build-and-test.md) and [Release operations](docs/releases.md) |
| Find engineering records | [Repository documentation index](docs/README.md) |
| Understand what belongs in this repository | [Repository audit](docs/repository-audit-20261019.md) |

## Develop from source

Use Linux or WSL with the dependencies in the [build guide](docs/build-and-test.md).
For a minimal host build and contract tests:

```bash
cmake --preset minimal-host
cmake --build --preset minimal-host -j
ctest --preset minimal-host
```

The `development-full` preset adds the image-processing and gateway dependencies;
`pi5-release` builds the Pi camera stack. Host tests do not exercise physical
cameras. Normal device installation uses the signed installer above.

### Repository layout

The main areas are the native engine (`apps/`, `src/`, `include/`, `platform/`),
device console (`web/`), client SDK (`sdk/`), installation (`tools/`, `deployment/`,
`debian/`), and engineering references (`docs/`, `tests/`). Normal installation
does not require browsing or building these folders.

<details>
<summary>Complete folder map (17 top-level directories)</summary>

| Path | Contents |
| --- | --- |
| `.github/` | GitHub Actions for builds, tests, release signing, and publication |
| `apps/` | Executable entry points for the depth daemon and secure web gateway |
| `cmake/` | Build metadata template for downstream native-library users |
| `config/` | Runtime defaults, calibration examples, and Pi trigger configuration |
| `debian/` | Debian package definitions, dependencies, install lists, and lifecycle hooks |
| `deployment/` | Systemd services, network discovery, asset provisioning, and release platform/signing configuration |
| `docs/` | Engineering references, setup specifications, research, and qualification evidence; user guides live in the [website wiki](https://docs.nuvohardware.com/vision-one/) |
| `include/` | C/C++ headers for runtime interfaces, data contracts, and the native SDK |
| `platform/` | Raspberry Pi camera/trigger support and Linux IPC integration |
| `presets/` | Candidate balanced, fast, and quality processing profiles |
| `protocol/` | Reference stream-metadata schema; the shipping gateway uses JSON metadata |
| `qualification/` | Raspberry Pi beta acceptance plan |
| `sdk/` | Python client, frame decoders, recording/replay, and package metadata |
| `src/` | Native processing, runtime, calibration, transport, and gateway implementations |
| `tests/` | Unit, integration, installer, SDK, web, package-hygiene, and hardware checks |
| `tools/` | Installation, provisioning, calibration, diagnostics, benchmarks, qualification, and release tools |
| `web/` | Device console, shared browser modules, and offline assets |

</details>

## License

Copyright © 2026 **NuvoHardware**.

Nuvo-authored software, including the runtime, web console, installer, tools,
and SDKs, is licensed under **GNU GPL version 3 or any later version**
(`GPL-3.0-or-later`). See [LICENSE](LICENSE) for the terms and [NOTICE](NOTICE)
for scope and third-party exceptions. Third-party software and assets retain
their own licenses and notices. The software is provided without warranty as
described in the license.

Published installer bundles include matching source and build instructions.
For rebuilding, redistribution, and installing modified software with your own
release key, see [Release operations](docs/releases.md).

## Development changes (unreleased)


- Production capture now requires the hardware-timed PIO trigger and validates
  negotiated image geometry and buffer counts before starting. Scheduler-timed
  GPIO is available only through the explicit capture-check diagnostic flag.
  See [capture diagnostics](docs/operations-troubleshooting.md).
- Automatic exposure holds a shared setting until a sustained lighting change,
  uses bounded adjustments, and waits for both cameras to report the requested
  controls. Persistent highlights no longer cause competing bright/dark updates.
  See [exposure controls](docs/config-schema.md).
- CPU rectification shares immutable maps and avoids staging already-owned
  memory. Independent image requests no longer repeat work on the matcher lane;
  frame pools have explicit checked budgets. See [runtime internals](docs/native-runtime-ipc-gateway.md).
- Unrectified color previews encode directly from I420. Optional preview failures
  stay isolated from capture, and timing separates preparation, queueing and
  encoding. See [performance measurement](docs/performance-profiles.md).
- Outputs now start with task presets and distinguish measurement arrays from
  compressed visual previews. SDK metadata identifies grids, units and capture
  lineage; browser decoding and reconnects have regression coverage.
  See [the console guide](docs/user-guide.md) and [SDK examples](docs/sdk-examples.md).

