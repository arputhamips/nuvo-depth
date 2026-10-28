# Minimal raw depth installation

The minimal package runs the existing native Pi capture and depth pipeline.
It provides terminal configuration and local raw depth output. It installs no
Nuvo web console, network SDK, Wi-Fi onboarding, calibration solver or encoder.
It selects the existing CPU rectifier and omits the optional GPU backend.
Native OpenCV and libcamera runtime dependencies are still required; distro
libraries can depend on additional image-format and camera support libraries.
The full and minimal packages are mutually exclusive.

Use a Raspberry Pi 5, the supported dual-IMX296 trigger head, and Raspberry Pi
OS Lite 64-bit Trixie. The currently qualified kernel is
`6.12.75+rpt-rpi-2712`; libcamera and IPA must match the package's pinned cohort.
Enable SSH and configure internet access before running the installer.
The bootstrap uses Python 3, which is included in the supported Pi OS image.
The installer retains your existing network and SSH configuration.

## Install the public preview

The approved public preview is recorded in `deployment/public-release.json`
(currently `v0.1.1-beta.8`). Its minimal bundle and installer are separate from
the full appliance downloads. The readiness/import commands below are included
in this public preview.

```bash
curl -fsSL https://raw.githubusercontent.com/arputhamips/nuvo-depth/main/install-nuvo-depth-minimal.sh | sudo bash
```

An initial install can require a reboot for camera overlays. Reconnect over SSH
after reboot and capture depth to finish the camera check. The package itself
does not change boot configuration or start capture until the installer is run.
Fresh beta 8 minimal installs on the supported head enable provisional preview,
with an explicit unverified-distance notice. Upgrades preserve the previous
capture policy, including beta 6's implicit preview and an existing deliberately
disabled policy. Use `--calibration-bundle /path/to/bundle` for a compatible
per-unit bundle or `--defer-capture` to leave a fresh installation disabled.
`--allow-provisional-preview` remains available as an explicit selection.
Reinstalls preserve existing calibration. See [readiness and import](calibration-readiness.md).

## Test with Actions artifacts

Download `nuvo-minimal-arm64` from a manual **ARM64 Debian packages** workflow
run with `package_profile=minimal`. Extract it, then on the Pi run:

```bash
sha256sum --check SHA256SUMS
sudo bash ./install-nuvo-depth-minimal.sh \
  --package ./nuvo-depth-minimal_0.1.1~beta.8_arm64.deb \
  --platform ./release-platform.json --dry-run
```

Use the actual `.deb` filename from the artifact. Remove `--dry-run` to install.
This local package path is for explicitly selected Actions artifacts and does
not claim release-signature verification. Published bundles use the existing
signed manifest and package-hash verification instead:

```bash
sudo bash ./install-nuvo-depth-minimal.sh --bundle ./extracted-minimal-bundle
```

## Configure and use

```bash
sudo nuvoctl configure
sudo nuvoctl config show
sudo nuvoctl config set exposure.manual_exposure_us 6000
sudo nuvoctl config set stereo.uniqueness_ratio 5
sudo nuvoctl config validate
sudo nuvoctl status
sudo nuvoctl calibration status
sudo nuvoctl restart
sudo nuvoctl depth capture --output depth.f32
sudo nuvoctl depth stream --frames 30 --hz 5 --output depth.nuv
```

Configuration changes to exposure/stereo controls reload while capture runs.
Status reports the requested/applied revisions and any daemon rejection.
The fixed camera geometry is `/etc/nuvo-depth/daemon.yaml`; changing its capture
dimensions also requires matching rectification data and a daemon restart.

The default one-frame capture writes row-major little-endian float32 optical-axis
Z depth in metres to `depth.f32`, associated validity bytes to
`depth.f32.validity.u8`, and the original frame metadata to `depth.f32.json`.
The metadata contains dimensions, frame identity, timestamp and calibration
authority. Invalid depth is NaN; validity code `1` identifies valid pixels.
Other validity codes preserve the pipeline's invalidity reason.
Existing output files are never overwritten.

Streams contain self-describing, length-delimited NuvoStream frames with only
depth and validity products. `--output -` writes binary NuvoStream to stdout;
diagnostics go to stderr. `--format nuv` is also available for one-frame capture.
The requested cadence is an upper bound, not a guaranteed matcher throughput.

The pipeline only computes depth while a reader requests it. An idle daemon
can report healthy capture with zero matcher work. Ctrl-C releases the reader's
subscription. Local access requires root or membership in `nuvo-video`; add an
application user with `sudo usermod -aG nuvo-video USER`, then log in again.

## Geometry and operation

The default geometry is the profile used on the unchanged rig with beta 5:
728 × 544, a measured 99.694 mm baseline, and a 441.792 px rectified focal
length. Its provenance and file hashes are recorded in
`config/calibration/nuvo_depth_100mm/profile.json`. Shared seed outputs retain
`provisional_recalibration_required` authority; these files do not establish
metric accuracy for a different assembled rig. Published beta 6 packages
predate this default change and still contain the earlier seed. There is no
calibration capture or solving workflow in this package. Public beta 8
supports existing-bundle import without editing service files:

```bash
sudo nuvoctl calibration import /path/to/bundle --dry-run
sudo nuvoctl calibration import /path/to/bundle
sudo nuvoctl calibration rollback
```

Use the original complete compatible per-unit bundle, never a relabelled seed.
Review the [validation, activation and recovery contract](calibration-readiness.md)
before importing. Older beta 6 lacks this command; upgrade within the minimal profile to the
approved public preview to use it.

```bash
sudo systemctl status nuvo-depthd-native.service
sudo journalctl -u nuvo-depthd-native.service -b
sudo nuvoctl stop
sudo nuvoctl start
```

## Build

Build on the ARM64 Trixie builder used by the existing packaging workflow:

```bash
NUVO_PACKAGE_PROFILE=minimal dpkg-buildpackage -b -us -uc
```

The default packaging command continues to build the full appliance packages.
For the minimal CMake variant use the `pi5-minimal` preset. No runtime capture,
matcher, rectification, depth conversion or IPC source is forked.

## Same-device ROS 2 from current source

The current source and its Actions packages prepare the local ROS connection as
part of minimal installation. The package creates the `nuvo-video` group and
the persistent `/run/nuvo-depth` directory with group access. The installer
enables the socket-activated native calibration control endpoint, including
when capture is deferred. It installs no Docker dependency.

```bash
sudo nuvoctl ros environment --json
```

This read-only command reports the host's numeric access GID, runtime directory,
data and calibration socket paths, package geometry capability, capture policy,
and current service/status checks. The same-device ROS launcher consumes these
values automatically, so a device-specific GID does not need to be copied into
a Docker command. The launcher mounts the runtime directory read-only and adds
the discovered numeric group. Native service stop/start preserves that directory
so an existing container mount sees replacement sockets. Fresh depth is still
verified by receiving frames; discovery does not subscribe or start capture.

An explicit ROS `calibration_file` request sends a complete compatible per-unit
bundle to the native control endpoint for validation and activation. Native
Nuvo Depth retains ownership of calibration, capture and settings. A normal
ROS connection leaves them unchanged, including a deliberately disabled
capture policy. A supplied bundle never grants factory authority or bypasses
device identity and geometry checks. See the ROS adapter's local-container
launcher documentation for its output/rate and provisional-preview choices.

These additions follow public `v0.1.1-beta.8`; that public bundle does not include
the new discovery/control endpoint or the frame calibration geometry needed by
the current ROS adapter. Use a matching current-source Actions package for
this path until a newer release is explicitly published. Installing the current
source or passing host/container tests does not establish Pi hardware
qualification or metric-accuracy acceptance.

## Finish first use

Keep upgrades within the installed profile. Removing the full packages and then
installing minimal is not a supported profile migration: locally retained full
service overrides and processes can survive removal and block minimal startup.
The cleaned-system qualification trial encountered this condition. Use a
separate supported OS image for a new minimal deployment; do not delete device
identity or calibration to force a profile change.

The maintained bootstrap resolves the approved public signed bundle for
this profile (as recorded in `deployment/public-release.json`, excluding drafts). It prints and binds
the exact tag before verifying the bundle, manifest and package cohort. Pass
`--release TAG` for a reproducible version, or use a release-specific installer.
Signature or discovery failures stop installation. Reruns preserve valid
per-unit calibration; an upgrade alone does not require recalibration.

Continue with the [complete first-depth journey](https://docs.nuvohardware.com/vision-one/guide/getting-started)
or [minimal terminal journey](https://docs.nuvohardware.com/vision-one/install/minimal).
