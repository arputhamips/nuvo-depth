# Install Nuvo Depth

## Prerequisites

- Raspberry Pi 5 with stable power and active cooling.
- Raspberry Pi OS Lite 64-bit, Debian Trixie (`arm64`).
- Supported kernel: `6.12.75+rpt-rpi-2712`.
- [Supported OS image and Pi setup](https://docs.nuvohardware.com/vision-one/install/byo-pi).
- Ethernet or Wi-Fi with internet access.
- Administrator access to a terminal on the Pi.
- [Current public preview](https://docs.nuvohardware.com/vision-one/releases/): `v0.1.1-beta.6`.

## Install

Choose one profile. The full appliance includes the browser interface and setup
workflow. The minimal profile provides raw depth and terminal configuration;
enable SSH and configure the Pi's network before installing it.
See [minimal installation and CLI usage](minimal-install.md).

```bash
# Full appliance
curl -fsSL https://github.com/arputhamips/nuvo-depth/releases/download/v0.1.1-beta.6/install-nuvo-depth.sh | sudo bash

# Minimal raw depth
curl -fsSL https://github.com/arputhamips/nuvo-depth/releases/download/v0.1.1-beta.6/install-nuvo-depth-minimal.sh | sudo bash
```
