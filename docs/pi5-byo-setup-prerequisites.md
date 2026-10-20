# Raspberry Pi 5 bring-up prerequisites for customer-owned boards

This document records the prerequisite state and setup order for running the
Nuvo Depth dual-IMX296 stack on a customer-owned Raspberry Pi 5. It is the
engineering source for a future customer wiki page and bootstrap script. It is
not a substitute for per-unit stereo calibration, electrical approval, or
production qualification.

## Clean-board reference state

The final 100 mm hardware was returned to a clean Raspberry Pi OS state on
2026-09-15 and reboot-verified before this checklist was written.

| Item | Verified clean state |
| --- | --- |
| Board | Raspberry Pi 5 Model B Rev 1.1, 16 GiB |
| OS | Debian 13 (trixie), 64-bit |
| Kernel | `6.12.75+rpt-rpi-2712` |
| Storage | 4.2 GiB used, 221 GiB available |
| Network | NetworkManager, Wi-Fi, SSH, and mDNS active |
| Camera runtime | `rpicam-apps` 1.12.0, `libcamera` 0.7.2 |
| Cameras | Two IMX296 color/RGGB sensors, 1456 x 1088, both detected |
| Camera paths | CAM/DISP0: `i2c@88000`; CAM/DISP1: `i2c@80000` |
| Boot camera policy | `camera_auto_detect=1`; no explicit IMX296 overlays |
| External trigger | Driver `trigger_mode=0`; GPIO17 is an input |
| Thermal state | 49.4 C shortly after reboot, `get_throttled=0x0` |
| Application state | No Docker, ROS, Hailo, Nuvo package/state, or test data |
| systemd | No failed units |
| dpkg | No audit errors |

The unprepared state above is deliberate. Both cameras must enumerate before
external-trigger configuration is applied. A bootstrap failure must not be
hidden by a pre-existing container, service, or camera process.

## Physical prerequisites

Before changing the OS, verify and record:

1. A Raspberry Pi 5 with a supported 64-bit Raspberry Pi OS/Debian image.
2. A stable Pi 5 power supply and active cooling suitable for sustained CPU
   load. Do not qualify from an undervolted or throttled board.
3. Two supported IMX296 color global-shutter modules connected to CAM/DISP0
   and CAM/DISP1 with undamaged, correctly oriented flex cables. Power must be
   disconnected before changing a camera cable.
4. A mechanically rigid stereo mount with the lens centres separated by the
   declared baseline. The production enclosure is nominally 100.0 mm, but the
   actual baseline belongs in the per-unit calibration result.
5. Both lenses focused and locked after final assembly. Replacing a sensor,
   lens, cable, mount, or focus setting invalidates the geometry profile.
6. Both camera XTR inputs wired to the same trigger signal, with a shared
   ground and board-specific electrical levels reviewed against the camera
   module documentation. Nuvo uses BCM GPIO17, physical header pin 11.
7. Recovery access: local console or a removable/reflashable SD card. A remote
   bootstrap must never be the only recovery path.
8. A known test scene, calibration target, oscilloscope or logic analyser, and
   a controlled non-PWM light source for final trigger/exposure qualification.

Software can confirm camera enumeration and frame response. It cannot prove
connector insertion, baseline metrology, electrical safety, optical focus, or
the physical pulse waveform.

## Clean OS prerequisites

Before installing Nuvo software:

- configure a unique OS user and password; do not ship a shared password;
- set the hostname and verify wired Ethernet or Wi-Fi connectivity;
- enable SSH only under the customer's security policy;
- set the correct locale, timezone, and NTP/time synchronisation;
- update the OS and reboot before freezing the software cohort;
- confirm that no other process owns either camera;
- keep USB gadget mode disabled until the product's power/data design, VID/PID,
  and supported hosts are approved.

Minimum baseline checks:

```bash
cat /proc/device-tree/model
uname -a
rpicam-hello --version
rpicam-hello --list-cameras
vcgencmd get_throttled
vcgencmd measure_temp
gpioinfo -c gpiochip0 17
```

`rpicam-hello --list-cameras` must report exactly two IMX296 RGGB cameras at
1456 x 1088. Stop if either sensor is absent, identifies as mono, repeatedly
disconnects, or reports a different readout geometry.

## Build and hardware-test packages

The production Debian package should eventually install runtime dependencies
from a signed Nuvo APT repository. Until that exists, a source build and the
Python hardware-in-loop tools require:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkgconf git rsync \
  libyaml-cpp-dev libopencv-dev \
  libavcodec-dev libavutil-dev libswscale-dev \
  libboost-system-dev libssl-dev libsystemd-dev \
  libegl-dev libgles-dev libcamera-dev \
  python3 python3-numpy python3-opencv python3-yaml \
  python3-cryptography python3-libcamera python3-picamera2 \
  python3-libgpiod gpiod v4l-utils raspi-utils rpicam-apps-lite \
  openssl avahi-daemon
```

Use `sudo apt build-dep ./` when building the Debian source package. Record the
resolved package versions with the qualification evidence; do not silently
upgrade a released device to a different libcamera, PiSP, Mesa, OpenCV, or
kernel cohort.

## External-trigger preparation

The native daemon requires explicit always-on camera overlays and the upstream
IMX296 external-trigger mode. The supported idempotent path is:

```bash
sudo nuvoctl platform preflight
sudo nuvoctl platform prepare --platform pi5 --apply
sudo reboot
sudo nuvoctl platform preflight
```

The resulting managed boot configuration is equivalent to:

```ini
camera_auto_detect=0
dtoverlay=imx296,always-on,cam0
dtoverlay=imx296,always-on,cam1
```

and the module policy is:

```text
options imx296 trigger_mode=1
```

After reboot, all of these must pass:

```bash
test "$(cat /sys/module/imx296/parameters/trigger_mode)" = 1
rpicam-hello --list-cameras
gpioinfo -c gpiochip0 17
```

The package service runs as `nuvo-depth`, with `video`, `render`, and `gpio`
supplementary groups. Do not run the production daemon as root merely to avoid
fixing device permissions.

## Trigger acceptance test

Run the repository's bounded verifier while all other camera services are
stopped:

```bash
sudo python3 tools/pi/verify_external_trigger.py \
  --output ./external-trigger.json \
  --left-camera-index 1 --right-camera-index 0 \
  --gpiochip /dev/gpiochip0 --line 17 \
  --low-pulse-ms 3 --priming-pulses 5 \
  --pulse-count 10 --pulse-period-ms 100 \
  --quiet-before-ms 500 --quiet-after-ms 500
```

Acceptance requires:

- zero frames during the quiet interval;
- both cameras respond to priming;
- exactly one new frame per camera per test pulse;
- the requested exposure/gain metadata is stable;
- sensor timestamp skew remains inside the product pairing limit;
- GPIO17 returns to the idle level and no process retains the line.

On the final hardware before it was cleaned, this test passed with zero idle
frames, 10/10 responses from each camera, approximately 3.08 ms userspace pulse
duration, 2,992 us reported exposure, and approximately 14–16 us left/right
sensor timestamp difference. This is functional evidence, not oscilloscope
evidence. A scope must still verify voltage, polarity, pulse width, edge
quality, and the relationship between XTR and actual exposure.

## Install and configure the Nuvo stack

1. Build natively with the `pi5-release` CMake preset or install one signed,
   version-locked arm64 package cohort.
2. Run every CTest before installing.
3. Provision the system users, runtime directories, device identity,
   certificate, and one-time claim token through package tools.
4. Install `daemon.yaml`, `stereo.json`, `exposure.json`, and `encoding.json` as
   one reviewed configuration cohort.
5. Keep encoder monitoring optional. Raw associated frame products are the
   authority; H.264/MJPEG is a downstream monitoring branch.
6. Run `depthd --check-config` before enabling services.
7. Enable the Nuvo target only after trigger verification passes.
8. Verify the native daemon owns both cameras and no ROS, rpicam, GStreamer, or
   browser helper competes for them.

The GPU path requires EGL/GLES and the Pi V3D driver. Rectification should
report `egl_gl_dmabuf`; a CPU fallback is useful for recovery but is not a
substitute for qualifying the production GPU path. OpenCV SGBM remains CPU
work on the current implementation.

## Provisional 100 mm test profile

There is no valid per-unit calibration for a newly assembled customer rig. For
engineering bring-up only, the existing 160 mm reference profile may seed a
provisional 100 mm profile by retaining its intrinsics and rectification
matrices and replacing only the stereo projection baseline:

```text
reference f  = 493.542046188 px
test B       = 0.100 m
right P[0,3] = -f * B = -49.3542046188 px*m
```

The reference projection encodes approximately 161.459 mm
(`79.6866968475 / 493.542046188`). The scaled profile must remain labelled
`provisional_recalibration_required`, and metric depth must remain disabled or
visibly marked engineering-only. Do not copy an empirical rectification
correction from a different physical assembly.

Camera order and orientation must be established on the assembled unit before
using the seed. The previous prototype used logical left index 1, logical right
index 0, and 180-degree orientation; that is evidence about the prototype, not
a universal property of customer wiring.

## Per-unit calibration boundary

Calibration is an advanced manufacturing/engineering process, not part of the
normal customer UI flow. Before calibrated metric depth is enabled:

- capture synchronized, unrectified images from the final focused assembly;
- solve a profile for every distinct sensor readout/crop/orientation geometry;
- bind the result to the unit and both stable camera identities;
- validate rectified vertical residual over the complete field;
- measure depth scale/bias and invalid coverage at known ranges;
- repeat after restart and thermal soak at every advertised frame rate;
- activate the content-addressed bundle only after independent acceptance.

See [Advanced calibration profile production](advanced-calibration.md).

## Performance and reliability qualification

The customer-owned board is not ready merely because one preview appears.
Retain machine-readable evidence for:

1. three capture restarts and at least 1,000 accepted pairs per cycle;
2. maximum sensor skew, rejected/stale pairs, buffer drops, and sequence gaps;
3. capture, GPU rectification, stereo preparation, matching, filtering, depth,
   encoding, gateway, and client-delivery timings;
4. process CPU, RSS, GPU path, output FPS, clock, temperature, fan behavior,
   and `get_throttled` before, during, and after load;
5. a sustained thermal/endurance soak, not just a short benchmark;
6. service restart, reboot, controlled power-cut, and state-preservation tests;
7. wired Ethernet throughput/loss and authenticated raw-frame decoding;
8. optional USB transport only after separate electrical and interoperability
   approval;
9. 60-frame aligned captures at ISP output, post-rectification, post-stereo,
   post-encoding, and receiving-PC decode when diagnosing flicker;
10. visual review of left/right focus, rectification, disparity smoothness,
    horizontal streaking, temporal stability, and depth plausibility.

Any nonzero current or historical throttle bits, repeated camera disconnects,
GPU fallback, trigger mismatch, unexplained frame gaps, or calibration failure
blocks release.

## What the bootstrap can automate

- OS/model/architecture/version preflight;
- dependency installation with a frozen package manifest;
- boot-config backup and idempotent managed trigger block;
- IMX296 module policy and reboot-required detection;
- camera count/type/path checks;
- service users, groups, tmpfiles, identity, and certificates;
- source/package hash verification, build, CTest, install, and rollback;
- bounded trigger, capture, pipeline, transport, and thermal tests;
- evidence collection with timestamps and hashes;
- removal of temporary build/test material.

## What must remain manual or independently witnessed

- safe cable installation and XTR wiring;
- electrical design approval and oscilloscope measurements;
- measured baseline, focus, mechanical rigidity, and enclosure inspection;
- calibration-target dimensions and capture-pose quality;
- independent metric-depth validation;
- cooling, airflow, power-supply, Ethernet, and USB physical acceptance;
- final release authorization.

The automation must fail closed, preserve the previous boot configuration, and
print an exact rollback command. It must never reformat storage, enable USB
gadget mode, or declare a provisional calibration production-valid.
