# Production deployment

For normal installation, use the [Vision One user wiki](https://docs.nuvohardware.com/vision-one/):
[bundled unit setup](https://docs.nuvohardware.com/vision-one/install/bundled-pi),
[your own Pi or SD card](https://docs.nuvohardware.com/vision-one/install/byo-pi),
and [first browser connection](https://docs.nuvohardware.com/vision-one/guide/getting-started).
This document is the engineering deployment reference.

The product package is a depth-only, systemd-managed Raspberry Pi 5 appliance
for the 100 mm dual-IMX296 rig. It contains no Hailo dependency, model artifact,
or semantic runtime. The native daemon alone owns the cameras and trigger. Local
clients use sealed frame descriptors; remote clients use the authenticated TLS
gateway over Ethernet, USB Ethernet, or explicitly bounded best-effort WiFi.

## Canonical paths

- Terminal/factory installation: [installer guide](../docs/install.md)
- Root-only setup, ownership and install receipts: `/var/lib/nuvo-depth-setup/`
- Native executables: `/usr/bin/depthd`, `/usr/lib/nuvo-depth/nuvo-gateway`
- Appliance configuration: `/etc/nuvo-depth/`
- Identity and calibration state: `/var/lib/nuvo-depth/`
- Status, snapshots, and local socket: `/run/nuvo-depth/`
- Web console and presets: `/usr/share/nuvo-depth/`
- Qualification plan: `/usr/share/nuvo-depth/qualification/pi5-beta.yaml`
- Native compatibility executable and diagnostic helpers: `/usr/lib/nuvo-depthd`.
- Verified calibration/configuration seed state: `/var/lib/nuvo-depthd`; asset
  verification and rollback remain supported. The parallel Python service is retired.

`/etc/nuvo-depth/encoding.json` controls the optional downstream H.264/MJPEG
stream. It can select raw-only, encoded-only, or simultaneous delivery and is
applied without restarting camera capture. Raw-only is the shipping default;
the encoder is a monitoring branch, not the authoritative depth transport. See the
[Pi 5 image-stream encoding guide](../docs/pi5-image-stream-encoding.md).

`/etc/nuvo-depth/stereo.json` controls the native SGBM/StereoBM matcher and
its full-resolution search/filter settings without restarting capture. See the
[60-frame flicker trace guide](../docs/pi5-fullres-stereo-flicker-trace.md).

`/etc/nuvo-depth/exposure.json` controls constant manual or shared stereo-auto
exposure, analogue gain, colour gains, metering limits, and settle cadence.
libcamera AE/AWB remains disabled; both cameras and the active-low external
trigger pulse receive the same requested exposure without restarting capture.
The effective shutter ceiling is the smaller of 8,000 us and one trigger
period less the 250 us scheduling guard. The web console shows the live limit;
the daemon rejects a command that would violate it.

For a bounded engineering check, run
`/usr/lib/nuvo-depthd/validate_exposure_live.py`. It switches to Auto,
samples unique published frames, and restores the exact starting document even
on failure. Run it as root so it can read runtime status and use the privileged
`nuvoctl` apply path. Its trigger-duration values are process-side diagnostics
and do not replace an electrical XTR measurement.

## Hardware trigger

On Pi5 the native service requires `nuvo-trigger-prepare.service`, which loads
the installed `pwm-pio` overlay for the configured trigger GPIO, exports its
single PWM channel, and grants the native group access to only that channel.
The daemon uses RP1 PIO PWM with an inverted 100 ms period and 8 ms low pulse
for the default 10 Hz/8 ms exposure. Exposure updates change hardware duty at
the next cycle. The existing GPIO17 wiring is retained.

The helper discovers the controller by device-tree compatibility and GPIO,
never by an assumed pwmchip number; it cannot select the native fan PWM.
A root-owned `/run/nuvo-depth-trigger/pwm` link supplies the one sysfs write
exception under `ProtectKernelTunables`. Preparation disables any retained owned
channel before a configuration change, and `ExecStopPost` disables the waveform
after normal exit, failed startup, SIGKILL or watchdog termination. The driver holds its last pin level
on disable, so the daemon stops both cameras before disabling PWM.
`rp1-pio-pwm` timing is commanded hardware timing, not an electrical measurement.
The userspace GPIO source remains a clearly labeled engineering fallback for
manual runs without the provisioned PIO channel.

See the [Raspberry Pi PIO driver overview](https://www.raspberrypi.com/news/piolib-a-userspace-library-for-pio-control/)
for the supported hardware PWM overlay.

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
depth semantics are missing.

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

The default seed uses the geometry that ran on the unchanged rig with beta 5:
728 × 544, a measured 99.694 mm baseline and a 441.792 px rectified focal length.
Its source bundle and file hashes are recorded in
`config/calibration/nuvo_depth_100mm/profile.json`. The shared seed remains
`provisional_recalibration_required`; a different mechanical assembly needs
its own validation before its metric range is treated as calibrated. Factory
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
sudo systemctl restart nuvo-system-agent.socket nuvo-depthd-native.service nuvo-gateway.service
sudo systemctl --no-pager --full status nuvo-depth.target
```

The privileged control socket is `/run/nuvo-system-agent.sock`, outside the
native daemon's `/run/nuvo-depth` directory. Its path must remain available
after depth-daemon restarts for web encoding and stereo controls.

Both full and minimal native units preserve the `/run/nuvo-depth` directory
inode across restart, failure recovery and an explicit stop/start. Ownership
remains `nuvo-depth:nuvo-video`, mode `0750`. This keeps an existing read-only
container directory bind mount connected to the replacement `data.sock`.
The units remove daemon-owned socket, status and fixed snapshot outputs before
startup and after shutdown, including failed startup. Other runtime files are
preserved. A stopped daemon therefore has no published socket or status file;
check the service journal for its final state. Reboot clears `/run` normally.

Pending calibration capture requests are retained across a short native restart.
Their caller still owns the reservation and timeout: normal completion consumes
the request, timeout removes the request and reservation, and native processing
checks that the matching reservation still exists before publishing. The service
does not delete requests or alter accepted per-unit calibration, capture policy,
or persistent state under `/var/lib/nuvo-depth`.

When upgrading from an older unit that deleted its runtime directory, recreate
the ROS container once after the native service is running so its bind mount
targets the current directory. Ordinary later native restarts need no container
restart. Mount the directory read-only with the numeric `nuvo-video` GID; avoid
mounting a single socket inode or all of `/run`.

`tests/native_runtime_directory_test.py --image nuvo-depth-ros:ci` exercises both
shipped units' lifecycle directives on an actual systemd host and a persistent
unprivileged Docker consumer. It checks restart, explicit stop/start, SIGKILL
recovery, fresh producer identities, cleanup, permissions and preservation of
unrelated files. It uses isolated scratch services and requires no cameras.

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
