# Native startup and SD compatibility validation — 28 October 2026

On the tested Pi 5, repeated trigger-startup failures were caused by SD/MMC
I/O stalls. Two subsequent normal reboots in SD high-speed compatibility mode
completed in about 53 seconds with zero native restarts and zero recorded MMC
errors. The second boot included the native source fixes at
`9b4be87882e82d0a529f827db7d566299a2997bd` and no temporary tracing.

This was an existing full-profile Pi OS Trixie installation on a 16 GB Pi 5,
kernel `6.12.75+rpt-rpi-2712`, with the actual dual-IMX296 camera and preserved
provisional calibration. It was not a fresh minimal/Lite installation or a
power-cycle endurance test. The published beta 9 packages and ROS v0.1.3 image
were not replaced with new releases.

## Evidence and changes

The original public-install reboot took 223.349 seconds and four native retries.
A diagnostic reboot reproduced three retries. A temporary sampler continued
every approximately 0.5 seconds while three 29–33 second intervals completed no
SD reads or writes and accumulated 94–98% CPU I/O wait. The native configuration
child and Docker waited in `filemap_fault`; PID 1 and ext4 journaling also waited
for storage. The trigger helper had not reached GPIO/PWM preparation.

The SD controller reported 20 recovered data timeouts although the journal had
no corresponding MMC I/O error. This establishes the blocking storage path;
it does not distinguish a card, host-controller, signalling or power defect.

The native changes reduce unnecessary startup work and expose failures:

- `depthd-config` links the existing canonical configuration parser, YAML and
  standard C/C++ runtime only. On the Pi its dependency listing has 7 entries,
  compared with 253 for `depthd`; effective JSON, digests, warnings and successful
  validation output matched byte-for-byte.
- Both full/minimal service preflights and trigger preparation use that utility.
  Trigger stages log flushed monotonic timestamps; its config child has a
  five-second timeout. Existing 20/30-second service deadlines are unchanged.
- `nuvoctl diagnostics` optionally reads existing MMC counters and exports only
  allowlisted integers. Recovered errors produce a warning; unavailable evidence
  is unknown. It does not mount debugfs, reset counters or change configuration.

## Reboot results

Times below are systemd's kernel plus userspace measurements, not the entire
remote disconnect interval. All rows used normal software reboots.

| Native software / SD mode | Boot time | Native restarts | MMC errors observed |
| --- | ---: | ---: | --- |
| Published beta 9, default SDR104 | 223.349 s | 4 | Counters not collected in this original boot |
| Published beta 9, diagnostic tracing, default SDR104 | 188.584 s | 3 | 20 data timeouts |
| Published beta 9, clock-only limit | 251.321 s | 4 | 18 command + 19 data + 6 request timeouts |
| Published beta 9, 3.3 V high-speed | 53.333 s | 0 | All 15 counters zero |
| Source fixes, 3.3 V high-speed, tracing removed | 52.639 s | 0 | All 15 counters zero |

The clock-only attempt was ineffective: requested clock was 50 MHz but actual
clock remained 200 MHz in SDR104 mode. Switching only the SD slot to 3.3 V
high-speed produced an actual 50 MHz clock. Wi-Fi retained its separate 1.8 V
DDR50 interface. The earlier long I/O-stall pattern was absent from the sampled
successful boot. Camera, networking and calibration settings were preserved.

On the final boot the configuration query took 0.395 seconds, trigger preparation
finished at 12.807 seconds and native became active at 29.421 seconds. The ROS
v0.1.3 container recovered automatically with zero restarts. Temporary sampler
and strace overrides were removed. Docker still took about 29 seconds to start
on this full desktop installation; minimal boot speed is not inferred from it.

Live ROS checks before and after the final reboot each verified 51 coherent
frame sets across depth, validity, CameraInfo, points, left/right images,
disparity and metadata in 20 seconds, at approximately 2.62–2.64 Hz with the
unchanged 10 Hz ceiling. Both checks added zero UDP receive-buffer errors.
Native trigger/frame-completion errors remained zero and matching returned to
idle after unsubscribe. Calibration, identity and configuration checks passed.

## Device-specific SD workaround

The tested [overlay source](../config/pi/nuvo-sd-highspeed-overlay.dts) targets
only the Pi 5 SD slot. It is **not enabled or installed by default**. Prefer
healthy supported storage and inspect controller errors before applying a
compatibility workaround. Zero observed errors do not prove a card healthy.

For this investigation the overlay was compiled with `dtc -@ -I dts -O dtb`,
applied offline to the selected kernel's DTB with `fdtoverlay`, then checked with
`fdtget` before enabling it for a reboot. The installed name is
`nuvo-sd-highspeed.dtbo`, in the active `overlay_prefix` directory, referenced by
`dtoverlay=nuvo-sd-highspeed` inside a labelled `NUVO SD COMPATIBILITY` block in
`config.txt`. A backup of the pre-test boot configuration was retained.

Changing only `max-frequency` is insufficient on the tested controller; the
`no-1-8-v` property is essential to the observed mode change. Do not apply a live
overlay to the mounted root storage device. To undo this device-specific
workaround, remove its labelled configuration block and reboot, preserving all
other camera and kernel settings. Revalidate it after changing the kernel,
board or storage. It reduces available SD throughput and is not a universal
camera requirement.

## Build and package checks

The configuration-enabled local build passed 56 tests. Both manual ARM64
artifact workflows passed for the exact source commit:

- [Full packages](https://github.com/arputhamips/nuvo-depth/actions/runs/40788326736):
  70/70 tests and package installation smoke checks.
- [Minimal package](https://github.com/arputhamips/nuvo-depth/actions/runs/40788331177):
  67/67 tests, dependency isolation and fresh runtime-only container installation.

The Pi tested a utility built from that commit and its matching helper, service
unit and diagnostics module. It did not install the entire newly built artifact
cohort. Host/container checks do not establish fresh minimal hardware acceptance.
The [ROS validation record](https://github.com/arputhamips/nuvo-depth-ros/blob/main/docs/validation-20261028.md)
records live topic checks and the earlier 25-case argument matrix.

Raw journals, traces, per-device identifiers and calibration material remain
private. This report makes no metric-accuracy, long-duration storage-health,
electrical-timing or production-qualification claim.
