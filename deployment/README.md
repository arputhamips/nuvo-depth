# Production deployment

The product package is a depth-only, systemd-managed Raspberry Pi 5 appliance
for the 100 mm dual-IMX296 rig. It contains no Hailo dependency, model artifact,
or semantic runtime. The native daemon alone owns the cameras and trigger. Local
clients use sealed frame descriptors; remote clients use the authenticated TLS
gateway over Ethernet or USB Ethernet.

## Canonical paths

- Native executables: `/usr/bin/depthd`, `/usr/lib/nuvo-depth/nuvo-gateway`
- Appliance configuration: `/etc/nuvo-depth/`
- Identity and calibration state: `/var/lib/nuvo-depth/`
- Status, snapshots, and local socket: `/run/nuvo-depth/`
- Web console and presets: `/usr/share/nuvo-depth/`
- Qualification plan: `/usr/share/nuvo-depth/qualification/pi5-beta.yaml`
- Legacy comparison service/assets: `/usr/lib/nuvo-depthd` and
  `/var/lib/nuvo-depthd`; these are not the new appliance API.

`/etc/nuvo-depth/encoding.json` controls the optional downstream H.264/MJPEG
stream. It can select raw-only, encoded-only, or simultaneous delivery and is
applied without restarting camera capture. See the
[Pi 5 image-stream encoding guide](../docs/pi5-image-stream-encoding.md).

`/etc/nuvo-depth/stereo.json` controls the native SGBM/StereoBM matcher and
its full-resolution search/filter settings without restarting capture. See the
[60-frame flicker trace guide](../docs/pi5-fullres-stereo-flicker-trace.md).

## Build the Debian package

Build on the frozen 64-bit Raspberry Pi OS cohort. Debian resolves the declared
OpenCV, libcamera, EGL/GLES, Boost, OpenSSL, systemd, Python, and calibration
dependencies:

```bash
sudo apt build-dep ./
dpkg-buildpackage --no-sign --build=binary
sudo apt install ../nuvo-depth_0.1.0_arm64.deb
```

For publication, build in a clean reproducible environment, generate and review
the SPDX artifact, sign the `.changes`/repository with the release identity,
and install from the staged repository on a clean Pi. Production configuration
fails closed if the Pi capture, GPU rectifier, gateway, systemd integration, or
depth semantics are missing. Semantic/Hailo targets are forbidden.

## First installation

Package installation creates the service accounts and device identity but does
not modify boot configuration or enable camera services. On an engineering
unit:

```bash
sudo nuvoctl platform preflight
sudo nuvoctl platform prepare --platform pi5 --apply
sudo reboot
sudo nuvoctl platform preflight
sudo systemctl enable --now nuvo-depth.target
```

The seed calibration is marked `provisional_recalibration_required`. It keeps
the existing camera-unit intrinsics and changes the projection to a nominal
100 mm baseline. Perform a new stereo calibration on the final mechanical
assembly before using metric range as calibrated product output. Factory
activation also requires a valid Ed25519 signature chained to the separately
provisioned public-key trust store. Remove the service's explicit provisional
engineering override before a customer release.

Do not enable USB gadget mode until the product's USB-C power design, approved
VID/PID, descriptors, and target hosts pass the qualification plan. Transport
changes made in the web console roll back automatically unless confirmed within
60 seconds.

## GPU rollback

The CPU rectifier remains installed. To select it without rebuilding:

```bash
sudo systemctl edit nuvo-depthd-native.service
```

```ini
[Service]
Environment=DEPTHD_RECTIFICATION_BACKEND=cpu_opencv
```

Run `sudo systemctl daemon-reload && sudo systemctl restart
nuvo-depthd-native.service`. Remove the drop-in to return to `egl_gl_dmabuf`. Runtime
GPU initialization/import failures also fall back to the CPU backend.

## Upgrade and rollback

After installing a new signed package cohort:

```bash
depthd --check-config /etc/nuvo-depth/daemon.yaml
sudo systemctl restart nuvo-depthd-native.service nuvo-gateway.service
sudo systemctl --no-pager --full status nuvo-depth.target
```

User calibration activation is immutable and independently reversible:

```bash
sudo systemctl stop nuvo-depthd-native.service
sudo -u nuvo-depth /usr/lib/nuvo-depth/nuvo-calibration rollback \
  --namespace user --state-root /var/lib/nuvo-depth
sudo systemctl start nuvo-depthd-native.service
```

Package rollback uses the previous signed APT version and must be exercised by
the arm64 lifecycle gate. Do not improvise a file-level downgrade over an
installed cohort.
