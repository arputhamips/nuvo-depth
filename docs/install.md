# Install Nuvo Depth

<!-- BEGIN APPROVED SETUP -->
Approved public release: **`v0.1.1-beta.8`**. Start with **Balanced**.
Use Raspberry Pi OS Lite 64-bit Trixie (`arm64`), kernel `6.12.75+rpt-rpi-2712`.
The rolling command follows this approved reference; development builds and
draft candidates are never selected automatically. Quality is an optional
profile with greater processing cost.
<!-- END APPROVED SETUP -->

## Prerequisites

- Raspberry Pi 5 with stable power and active cooling.
- Raspberry Pi OS Lite 64-bit, Debian Trixie (`arm64`).
- Supported kernel: `6.12.75+rpt-rpi-2712`.
- [Supported OS image and Pi setup](https://docs.nuvohardware.com/vision-one/install/byo-pi).
- Ethernet or Wi-Fi with internet access.
- Administrator access to a terminal on the Pi.
- [Current public preview](https://docs.nuvohardware.com/vision-one/releases/): `v0.1.1-beta.8`.

## Install

Choose one profile. The full appliance includes the browser interface and setup
workflow. The minimal profile provides raw depth and terminal configuration;
enable SSH and configure the Pi's network before installing it.
See [minimal installation and CLI usage](minimal-install.md).

```bash
# Full appliance
curl -fsSL https://raw.githubusercontent.com/arputhamips/nuvo-depth/main/install-nuvo-depth.sh | sudo bash

# Minimal raw depth
curl -fsSL https://raw.githubusercontent.com/arputhamips/nuvo-depth/main/install-nuvo-depth-minimal.sh | sudo bash
```

## Finish first use

The maintained bootstrap resolves the approved public signed bundle for
this profile (as recorded in `deployment/public-release.json`, excluding drafts). It prints and binds
the exact tag before verifying the bundle, manifest and package cohort. Pass
`--release TAG` for a reproducible version, or use a release-specific installer.
Signature or discovery failures stop installation. Reruns preserve valid
per-unit calibration; an upgrade alone does not require recalibration.

Continue with the [complete first-depth journey](https://docs.nuvohardware.com/vision-one/guide/getting-started)
or [minimal terminal journey](https://docs.nuvohardware.com/vision-one/install/minimal).
