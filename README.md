# Nuvo Depth

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

## What is included

- **Camera and depth runtime:** synchronized capture, image rectification,
  stereo matching, and associated image, disparity, depth, confidence, and
  validity outputs.
- **Web console:** device setup, health, live previews, camera controls, and
  calibration tools, accessed through an authenticated HTTPS gateway.
- **Application interfaces:** C and Python SDKs, local IPC, and network delivery
  for integrating camera output into your application.
- **Installation and maintenance:** terminal setup, protected Wi-Fi setup
  hotspot, system services, signed Debian packages, and GitHub release builds.

## Get started

- **Preinstalled Vision One:** [Connect and set up the device](https://docs.nuvohardware.com/vision-one/install/bundled-pi).
- **Your own Pi or SD card:** use a Raspberry Pi 5 with Raspberry Pi OS Lite
  64-bit Trixie (`arm64`), kernel `6.12.75+rpt-rpi-2712`, internet access and
  administrator access.
- [Supported OS image](https://docs.nuvohardware.com/vision-one/install/byo-pi).
- **Current public preview:** `v0.1.1-beta.4`.

```bash
curl -fsSL https://github.com/arputhamips/nuvo-depth/releases/download/v0.1.1-beta.4/install-nuvo-depth.sh | sudo bash
```

## Current status

- The public preview has signed packages and tested installation, hotspot,
  network confirmation and owner-login flows.
- [Qualification evidence](docs/release-qualification-20261019.md) records the
  tested scope; accepted per-unit calibration is required for metric depth.
- Routine testing uses Actions artifacts. New release candidates are prepared
  manually as drafts and published only after approval.
- [Changelog](CHANGELOG.md) and [release process](docs/releases.md).

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
| `web/` | Device console, shared browser modules, offline assets, and parked editor prototype source |

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
