# Raspberry Pi 5 full-stack deployment record — 2026-09-15

This is the verified engineering deployment record for the clean production
hardware at `nuvomow.local`. It is also the source document for the customer
wiki's Raspberry Pi 5 installation page.

The stack is installed, enabled, and running after a reboot. Capture, external
triggering, EGL/GLES rectification, CPU SGBM, raw delivery, optional H.264
monitoring, the authenticated HTTPS gateway, Web UI, and live controls were
tested on the device. The current calibration is deliberately provisional, so
this deployment does **not** authorize production metric depth until the final
100 mm assembly has been calibrated and its signed profile activated.

## Verified result

| Item | Result |
| --- | --- |
| Board | Raspberry Pi 5 Model B Rev 1.1, 16 GiB |
| OS | Debian 13 (trixie), 64-bit |
| Kernel | `6.12.75+rpt-rpi-2712` |
| Cameras | Two IMX296 RGGB sensors, 1456 x 1088 |
| Trigger | Active-low BCM GPIO17, physical header pin 11 |
| Trigger acceptance | 10/10 frames from each camera; 13–14 us sensor timestamp separation |
| Installed Nuvo packages | Eight runtime packages, version `0.1.0` |
| Build tests | 43/43 passed on the Pi |
| Native target | Enabled and active after reboot; zero service restarts |
| Rectification | `egl_gl_dmabuf`, no fallback reason |
| Stereo matcher | OpenCV CPU SGBM, HH aggregation |
| Deployed output geometry | 728 x 544 image and disparity/depth products |
| Raw HTTPS frame | 4,753,427 bytes, correct Nuvo content type |
| Optional encoder | H.264 enabled transiently, encoded frame fetched, then restored to raw-only |
| 60-frame bounded run | Passed in 19.062 s; 3.148 published FPS |
| Temperature during bounded run | 58.95–60.05 C, 59.62 C average |
| Throttling | `0x0` throughout build, test, load, and reboot checks |
| Gateway and UI controls | Authentication, CSRF, reads, and stereo/exposure/encoding writes passed |

The 60-frame result is a count of **published depth frames**, not a claim of
60 FPS depth. In this profile the CPU SGBM matcher is the limiting stage.

## Release boundary

This installation is a complete engineering stack, but it is not yet a
customer release image:

- the package cohort was built from the current working tree and is unsigned;
- the included 100 mm profile is marked
  `provisional_recalibration_required` and runs only because the service has
  the explicit engineering `--allow-provisional-depth` flag;
- depth scale and absolute range have not been accepted against the final
  100 mm mechanics;
- a sustained thermal/endurance soak and signed APT upgrade/rollback test are
  still release gates;
- raw-only is the shipping default; H.264 is an optional monitoring branch.

Do not describe this unit as factory-calibrated or its metric depth as
production-authoritative until the calibration gate is complete.

## Source and package provenance

The final deployed-source archive is:

```text
/home/nuvo/nuvo-depth-deployed-source-20260915.tar.gz
SHA256 c98c83146c6c549b0a7c2bbaf2a4acc1afd14b4ffa26b6106b601732f594e6d8
```

The build/evidence root retained on the Pi is:

```text
/home/nuvo/nuvo-depth-deploy-20260915.42Tyer
```

The final runtime package hashes are:

```text
61bc2d9b788d4395573a94f5120ff6154cb840ef7802e5b721c1ebaceb8af8c7  nuvo-depth_0.1.0_arm64.deb
9084e7f17f7c20703b521b714a3ee5c6f207e6dd763b04b08083595007d0f639  nuvo-depth-core_0.1.0_arm64.deb
c85977a542e6c3bf10b45e94a01a6c2261e0051cd4d4718f3314d040ca7f8d05  nuvo-depth-platform-pi5_0.1.0_arm64.deb
d54f306a97a101a8a4870fa5ff90e73351e463ae30c82dfb0caf07e0076a99f6  nuvo-depth-gateway_0.1.0_arm64.deb
f145424c2cecd4324de3dd3792ba9ee80c3efb5d2c02c06311236052f8c1be2d  libnuvo-depth1_0.1.0_arm64.deb
2637594563954caaceab706aacd872c12496a605c7037574a93853aed38ba45d  python3-nuvo-depth_0.1.0_all.deb
06bff7077bd37e9008174091dda31440bfb053e98ea8ed534826e2b165dac928  nuvo-depth-calibration_0.1.0_all.deb
5a098b03c4e349dee58bfdac0edc3350ba40ea2223c85e75b3c5913d89a2becc  nuvo-depth-tools_0.1.0_all.deb
```

The development, debug-symbol, and factory-only packages were not installed.
Docker, ROS, Hailo, and old robot data were not reintroduced.

## Clean-board prerequisites

Before installation:

1. Use a supported 64-bit Raspberry Pi OS/Debian image on a Raspberry Pi 5.
2. Configure the customer account, hostname, network, time, SSH policy, stable
   Pi 5 power supply, and active cooling.
3. Power the board down before installing or changing either camera cable.
4. Install both supported IMX296 color sensors and shared XTR wiring.
5. Connect both XTR inputs to BCM GPIO17 with a shared ground and approved
   electrical levels.
6. Confirm the final mechanical baseline, focus, cable retention, and thermal
   assembly. These cannot be certified by software.
7. Update the OS, reboot, and confirm no camera-owning service is running.

Minimum clean-board checks:

```bash
cat /proc/device-tree/model
cat /etc/os-release
uname -a
rpicam-hello --version
rpicam-hello --list-cameras
gpioinfo -c gpiochip0 17
vcgencmd measure_temp
vcgencmd get_throttled
systemctl --failed --no-pager
dpkg --audit
```

Stop if both 1456 x 1088 IMX296 RGGB cameras are not present, if the board has
current or historical throttle bits, or if another process owns a camera.

## Install build and hardware-test dependencies

The source build used this dependency set:

```bash
sudo apt-get update
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential cmake ninja-build pkgconf git rsync devscripts debhelper \
  dpkg-dev fakeroot \
  libyaml-cpp-dev libopencv-dev \
  libavcodec-dev libavutil-dev libswscale-dev \
  libboost-system-dev libssl-dev libsystemd-dev \
  libegl-dev libgles-dev libcamera-dev \
  python3 python3-numpy python3-opencv python3-yaml \
  python3-cryptography python3-libcamera python3-picamera2 \
  python3-libgpiod gpiod v4l-utils raspi-utils rpicam-apps-lite \
  openssl avahi-daemon
```

For a customer release, these build dependencies belong in a clean package
builder. A customer board should install the signed runtime cohort from the
Nuvo APT repository instead of compiling locally.

## Verify and unpack the source

```bash
cd /home/nuvo
printf '%s  %s\n' \
  c98c83146c6c549b0a7c2bbaf2a4acc1afd14b4ffa26b6106b601732f594e6d8 \
  nuvo-depth-deployed-source-20260915.tar.gz | sha256sum --check

deploy_root="$(mktemp -d /home/nuvo/nuvo-depth-deploy-XXXXXXXX)"
mkdir -p "$deploy_root/source" "$deploy_root/evidence"
tar -xzf nuvo-depth-deployed-source-20260915.tar.gz \
  -C "$deploy_root/source"
cd "$deploy_root/source"
```

Never build or install an archive whose checksum does not match the release
manifest.

## Build and test the Debian cohort

```bash
export DEB_BUILD_OPTIONS=parallel=4
set -o pipefail
dpkg-buildpackage --no-sign --build=binary -j4 \
  2>&1 | tee "$deploy_root/evidence/dpkg-buildpackage.log"
sha256sum "$deploy_root"/*.deb \
  | tee "$deploy_root/evidence/SHA256SUMS"
```

`dpkg-buildpackage` runs CTest. The verified build passed 43/43 tests in 7.80
seconds. A failed unit/integration test blocks installation.

The final package intentionally excludes the retired Python
`nuvo-depthd.service` and its obsolete `cam`-owned tmpfiles rule. Production
uses `nuvo-depthd-native.service`, the `nuvo-depth` service account, and
`/usr/lib/tmpfiles.d/nuvo-depth.conf`. The source changes are recorded in:

- `debian/nuvo-depth-core.install`
- `debian/not-installed`

This prevents a harmless but misleading “user cam not found” boot warning and
avoids shipping two competing camera services.

## Install the runtime packages

Install one hash-verified cohort together:

```bash
cd "$deploy_root"
sudo apt-get install -y \
  ./libnuvo-depth1_0.1.0_arm64.deb \
  ./nuvo-depth-calibration_0.1.0_all.deb \
  ./nuvo-depth-core_0.1.0_arm64.deb \
  ./nuvo-depth-gateway_0.1.0_arm64.deb \
  ./nuvo-depth-platform-pi5_0.1.0_arm64.deb \
  ./nuvo-depth-tools_0.1.0_all.deb \
  ./python3-nuvo-depth_0.1.0_all.deb \
  ./nuvo-depth_0.1.0_arm64.deb
```

Package installation provisions service users, runtime/state directories,
device identity, a local TLS certificate, and a claim token. Never put the
claim token or private key in a build log, wiki, support bundle, or source
archive.

Validate the installed configuration before touching boot state:

```bash
depthd --check-config /etc/nuvo-depth/daemon.yaml
sudo nuvoctl platform preflight
```

The clean board is expected to fail the first trigger-ready preflight because
the required always-on overlays and trigger-mode module policy are not present
yet.

## Prepare the Pi camera platform

Apply the idempotent managed changes and reboot:

```bash
sudo nuvoctl platform prepare --platform pi5 --apply
sudo reboot
```

The tool adds this managed boot block to `/boot/firmware/config.txt` and keeps
the original as `config.txt.nuvo-backup`:

```ini
# BEGIN NUVO DEPTH (managed)
camera_auto_detect=0
dtoverlay=imx296,always-on,cam0
dtoverlay=imx296,always-on,cam1
# END NUVO DEPTH (managed)
```

It also creates `/etc/modprobe.d/nuvo-depth-imx296.conf`:

```text
options imx296 trigger_mode=1
```

After reboot:

```bash
sudo nuvoctl platform preflight
test "$(cat /sys/module/imx296/parameters/trigger_mode)" = 1
rpicam-hello --list-cameras
gpioinfo -c gpiochip0 17
vcgencmd get_throttled
```

The verified post-reboot preflight reported every check true and
`reboot_required=false`.

## Run the bounded trigger acceptance test

Run this before enabling the appliance target so no other process owns the
cameras:

```bash
sudo python3 tools/pi/verify_external_trigger.py \
  --output ./external-trigger-after-install.json \
  --left-camera-index 1 --right-camera-index 0 \
  --gpiochip /dev/gpiochip0 --line 17 \
  --low-pulse-ms 3 --priming-pulses 5 \
  --pulse-count 10 --pulse-period-ms 100 \
  --quiet-before-ms 500 --quiet-after-ms 500
```

This device passed with:

- `trigger_mode=1` and active-low GPIO17;
- 10/10 new frames from each camera;
- 2,992 us reported exposure;
- approximately 3.061–3.086 ms measured userspace low pulses;
- 13–14 us left/right sensor timestamp separation;
- no unexpected quiet-window frames.

This proves bounded functional response, not electrical waveform quality. Use
an oscilloscope for production XTR polarity, voltage, edge, pulse-width, and
exposure-relative timing acceptance.

## Enable and start the appliance

```bash
sudo systemctl enable --now nuvo-depth.target
sleep 10
systemctl --no-pager --full status \
  nuvo-depth.target \
  nuvo-depthd-native.service \
  nuvo-gateway.service \
  nuvo-system-agent.socket \
  nuvo-transport-rollback.timer
systemctl --failed --no-pager
ss -lnt | grep ':7443'
```

The target starts:

- `nuvo-depthd-native.service` for dual-camera ownership, trigger generation,
  rectification, stereo, depth, and snapshot publication;
- `nuvo-gateway.service` for authenticated TLS, Web UI, raw/encoded delivery,
  and settings APIs;
- `nuvo-system-agent.socket` for narrowly scoped privileged settings writes;
- `nuvo-transport-rollback.timer` for fail-safe transport changes.

The target is enabled; the child units are intentionally pulled in by the
target rather than independently enabled.

## Access and operating defaults

The local Web UI is available at:

```text
https://nuvomow.local:7443/
```

Use the unit's claim workflow to authenticate. The current engineering
certificate is locally provisioned, so a browser may require an explicit trust
step until production PKI is configured.

Canonical paths:

| Purpose | Path |
| --- | --- |
| Daemon configuration | `/etc/nuvo-depth/daemon.yaml` |
| Stereo controls | `/etc/nuvo-depth/stereo.json` |
| Exposure controls | `/etc/nuvo-depth/exposure.json` |
| Encoder controls | `/etc/nuvo-depth/encoding.json` |
| Identity and credential state | `/var/lib/nuvo-depth/identity/` |
| Runtime status | `/run/nuvo-depth/status.json` |
| Latest raw frame | `/run/nuvo-depth/snapshots/latest.nuv` |
| Latest encoded frame | `/run/nuvo-depth/snapshots/latest.nve` |
| Web assets | `/usr/share/nuvo-depth/web/` |

The verified operating default is:

- raw data delivery enabled;
- H.264/JPEG monitor encoding disabled;
- manual 8,000 us exposure and 2.0 analogue gain;
- SGBM HH with validity-preserving median filtering;
- 728 x 544 capture and matching output;
- EGL/GLES DMA-BUF rectification;
- provisional 100 mm engineering calibration.

The Web UI can select raw-only, encoded-only, or both. Keep raw enabled for
authoritative processing; encoded video is for monitoring.

## Acceptance checks used on this deployment

The authenticated gateway check performed all of the following:

1. created an authenticated cookie session and received a CSRF token;
2. read health, capabilities, transport, stereo, exposure, and encoding state;
3. posted unchanged stereo, exposure, and encoder settings and verified that
   the persisted response matched;
4. downloaded a 4,753,427-byte raw frame with content type
   `application/vnd.nuvo.frame-set`;
5. changed the encoder from raw-only to simultaneous raw + H.264;
6. downloaded a valid encoded frame with content type
   `application/vnd.nuvo.encoded-image`;
7. restored and re-read raw-only mode;
8. fetched the Web UI HTML successfully.

The same check passed again after a full reboot. No credential value is stored
in the acceptance evidence.

Useful operator checks:

```bash
sudo cat /run/nuvo-depth/status.json | python3 -m json.tool
systemctl show -p NRestarts -p MainPID -p ActiveEnterTimestamp \
  nuvo-depthd-native.service nuvo-gateway.service
journalctl -b -u nuvo-depthd-native.service -u nuvo-gateway.service \
  --no-pager
vcgencmd measure_temp
vcgencmd get_throttled
```

## Measured pipeline and resource use

The bounded runtime test waited for 60 newly published outputs and sampled the
processes once per second:

| Measurement | Result |
| --- | ---: |
| Duration | 19.062 s |
| Published output rate | 3.148 FPS |
| Capture/paired inputs in window | 192 |
| Dropped/replaced input opportunities | 132 |
| EGL/GLES rectification | 7.798 ms |
| Stereo preprocessing | 0.165 ms |
| CPU SGBM match | 295.980 ms |
| Stereo postprocessing | 3.692 ms |
| Total processing | 309.961 ms |
| Valid disparity fraction | 0.4175 |
| Maximum live pair delta | 51 us |
| `depthd` CPU | 100.5% average on one-core scale |
| `depthd` RSS | 314 MiB average, 355 MiB maximum |
| Gateway CPU | 0.16% average |
| Gateway RSS | 37.1 MiB |
| Temperature | 59.62 C average, 60.05 C maximum |
| Throttling | `0x0` on every sample |

Hardware assignment in the current pipeline:

- the two IMX296 sensors and Pi CFE/PiSP perform sensor capture and ISP output;
- BCM GPIO17 generates the shared active-low XTR pulse;
- V3D through EGL/GLES performs rectification via DMA-BUF;
- CPU/OpenCV performs SGBM HH, disparity filtering, and depth conversion;
- FFmpeg/libx264 with ARM NEON performs optional H.264 monitoring;
- the gateway is CPU/network I/O but was effectively idle in the bounded test.

The 296 ms SGBM match consumes almost the entire current frame budget and is
the reason the output is approximately 3.15 FPS. Rectification, gateway, and
thermals are not the limiting stages. The drop-latest policy deliberately
keeps output current while replacing unprocessed pairs. Achieving materially
higher depth FPS requires matcher optimization or a different accelerated
matcher; enabling more trigger pulses alone will only increase replacements.

## Reboot persistence result

After a final reboot:

- `nuvo-depth.target` was enabled and active;
- daemon, gateway, control socket, and rollback timer were active;
- daemon and gateway each reported `NRestarts=0`;
- `trigger_mode` was still `1`;
- status reported running capture, EGL/GLES rectification, and CPU SGBM;
- authenticated API, raw frame, encoder toggle, and Web UI checks passed again;
- temperature was 60.9 C and throttling remained `0x0`.

The removed legacy `cam` tmpfiles warning did not recur. Remaining warning-level
messages are Raspberry Pi OS/base-image items: Netplan file permissions,
Bluetooth configuration/regulator messages, ALSA udev rules, Wi-Fi scanning,
and the IMX296 link-frequency estimate. They are not Nuvo service failures but
should be handled in the production image hardening checklist.

## Evidence index

The copied evidence is under
`deliverables/pi5-full-stack-deployment-20260915/evidence/`:

- `dpkg-rebuild-final.log` — final build and 43/43 test output;
- `runtime-package-sha256-final.txt` — installed runtime package hashes;
- `core-package-final.contents` — proof that the legacy unit/tmpfiles rule is absent;
- `external-trigger-after-install.json` — bounded 10-pulse trigger result;
- `systemd-start-status.txt` — initial live service state;
- `api-acceptance.json` — authenticated gateway/UI/raw/encoded control test;
- `api-acceptance-after-reboot.json` — the same acceptance test after reboot;
- `runtime-60-frame-acceptance.json` — performance, memory, temperature, and throttle data;
- `status-after-reboot.json` — native daemon status after persistent startup;
- `journal-warning-or-higher-after-reboot.txt` — final warning inventory;
- `deployed-source-sha256.txt` — source archive integrity proof.

## Remaining release work

Priority order:

1. Generate and independently validate the per-unit 100 mm calibration, then
   remove the provisional-depth service override.
2. Decide the production depth-rate target and optimize/replace CPU SGBM if
   approximately 3.15 FPS does not meet it.
3. Run the full thermal/endurance, power-cycle, camera-restart, Ethernet, and
   support-bundle qualification plan.
4. Build the cohort in a clean reproducible builder, sign it, publish it in the
   staged Nuvo APT repository, and exercise upgrade and rollback.
5. Fix or explicitly accept the remaining base-image hardening warnings.
6. Provision production certificates/claim workflow and remove all shared
   setup credentials before delivery.
7. Keep USB gadget transport disabled until its electrical design, VID/PID,
   descriptors, power behavior, and target-host matrix are approved.

## Related engineering documents

- [Raspberry Pi 5 bring-up prerequisites](pi5-byo-setup-prerequisites.md)
- [Advanced calibration profile production](advanced-calibration.md)
- [Operations and troubleshooting](operations-troubleshooting.md)
- [User guide](user-guide.md)
- [Pi 5 image-stream encoding](pi5-image-stream-encoding.md)
- [Debian/APT publication roadmap](debian-archive-roadmap.md)
