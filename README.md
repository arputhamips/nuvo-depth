# Nuvo Depth

Nuvo Depth is the software that powers **Nuvo Vision One**, NuvoHardware's
stereo vision camera. It captures synchronized images from two cameras,
computes depth, and provides a web console and APIs for robotic applications.

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

**Have a preinstalled Vision One?** Follow the
[Vision One setup guide](https://docs.nuvohardware.com/vision-one/guide/getting-started)
to connect the device, complete network setup, and create your owner login.

**Using your own Pi or SD card?** Prepare the supported Raspberry Pi OS Lite
64-bit Trixie installation, then run this in an interactive terminal **on the Pi**:

```bash
curl -fsSL https://github.com/arputhamips/nuvo-depth/releases/download/v0.1.1-beta.4/install-nuvo-depth.sh | sudo bash
```

The installer verifies and installs the complete package set. It asks whether
to keep the current network or use the setup hotspot afterward, then collects
device, calibration, and owner-login choices. Open the console URL it prints
when installation finishes.

Read the [Pi installation guide](https://docs.nuvohardware.com/vision-one/install/byo-pi)
first for the exact supported OS/kernel, hardware requirements, and setup steps.
No GitHub account, SSH deploy key, or individual package selection is required.

## Current status

The current install release is **v0.1.1-beta.4**, a signed beta. Terminal
installation, first-boot hotspot setup, network confirmation, and owner login
have been exercised on the qualification Pi. See the
[release and setup qualification record](docs/release-qualification-20261019.md)
for the scope of those checks.

The checked-in calibration is provisional. Production metric-depth accuracy
requires accepted per-unit calibration and physical validation. Fresh-SD
lifecycle, electrical and USB power/data, wired throughput, and endurance
acceptance remain separate release gates.

## Documentation

The **[Nuvo Hardware wiki](https://docs.nuvohardware.com/vision-one/)** is the
main user documentation. This repository retains versioned technical references
and test evidence.

| You want to… | Start here |
| --- | --- |
| Set up and use Vision One | [Setup guide](https://docs.nuvohardware.com/vision-one/guide/getting-started) |
| Install on your Pi | [Pi installation guide](https://docs.nuvohardware.com/vision-one/install/byo-pi) |
| Integrate camera output | [SDK guide](https://docs.nuvohardware.com/vision-one/sdk/) and [Python SDK reference](sdk/python/README.md) |
| Configure terminal or factory provisioning | [Installer reference](docs/install.md) |
| Understand the runtime and repository | [Image pipeline and repository map](docs/image-pipeline-map.md) |
| Build or publish packages | [Build and test](docs/build-and-test.md) and [Release operations](docs/releases.md) |
| Find engineering records | [Repository documentation index](docs/README.md) |

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

| Path | Contents |
| --- | --- |
| `apps/`, `src/`, `include/` | Native daemon, gateway, processing, and public interfaces |
| `platform/` | Raspberry Pi camera, trigger, and Linux integration |
| `web/` | Device web console |
| `sdk/`, `protocol/` | SDKs and wire-protocol definitions |
| `config/`, `presets/` | Runtime configuration and reference profiles |
| `tools/` | Installation, calibration, diagnostics, and release tools |
| `debian/`, `deployment/` | Packages, services, and release configuration |
| `tests/`, `docs/` | Automated checks, specifications, and qualification evidence |

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
