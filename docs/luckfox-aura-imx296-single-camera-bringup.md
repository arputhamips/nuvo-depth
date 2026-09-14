# Luckfox Aura single-IMX296 firmware and bring-up plan

## Objective

Establish a recoverable, reproducible Luckfox Aura firmware that can operate one
finished Raspberry Pi-compatible IMX296 camera module on the Aura `CSI0`/H3
connector. Prove normal free-running raw capture before introducing the second
camera, external trigger, stereo pairing, calibration, or the Nuvo depth daemon.

The first-camera milestone is deliberately below the final product stack:

```text
official Aura image
  -> reproducible unmodified Aura SDK image
  -> IMX296 probes on CSI0/I2C4
  -> complete sensor-to-RKCIF media graph
  -> stable native-resolution RAW capture
  -> stable 728 x 544 / 10 Hz RAW capture
  -> freeze the known-good single-camera firmware
  -> begin dual-camera work
```

## Fixed decisions

- Target OS: **Luckfox Aura Debian 13**. This matches the future apt-package
  product direction.
- Target board: user-confirmed **4 GB RAM, MicroSD-only Luckfox Aura** with a
  32 GB MicroSD card. There is no eMMC on this unit.
- Build host: dedicated **Ubuntu 22.04 x86-64**, the environment officially
  supported by the Aura SDK. Do not make WSL the reference build environment.
- First connector: **CSI0/H3** only. The schematic routes its control bus to
  RV1126B I2C4 and provides 4.7 kOhm pull-ups to 3.3 V.
- First sensor mode: free-running. Do not connect or enable XTR yet.
- First data path: sensor RAW through the CSI/D-PHY/RKCIF path. ISP color tuning
  is not required to prove normal sensor operation.
- Initial module assumption: a finished 15-pin Raspberry Pi-compatible IMX296
  module, not a bare sensor board. A generic 22-pin-to-15-pin cable is only a
  candidate until every relevant signal has been mapped and verified by
  continuity; connector pitch alone is not evidence of compatibility.
- Preserve the vendor board DTS. Add a Nuvo-specific single-camera DTS/DTSI and
  select it from a copied board configuration instead of overwriting the only
  known-good vendor configuration.
- Preserve the selected Debian target's actual CMA setting during initial
  bring-up. The public 128 MB example belongs to a Buildroot configuration and
  is not a fixed Debian requirement.

## Stop conditions before connecting the camera

Do not power the camera until all of these are true:

1. The exact camera module manufacturer, model, PCB revision, and color/mono
   variant have been recorded, with clear photographs of both sides of the
   module, the Aura PCB/SKU markings, and both cable ends.
2. The module is a completed Raspberry Pi-compatible camera board, not a bare
   sensor breakout requiring separate 1.2 V and 1.8 V supplies.
3. Cable continuity and orientation have been checked by signal, not by an old
   pin table or connector appearance. The supplied Aura schematic labels H3 pin
   1 as 3.3 V, pin 22 as ground, pin 2 as CSI0/I2C4 SDA, and pin 3 as
   CSI0/I2C4 SCL. The physical ribbon numbering and mating view must still be
   confirmed on the actual board. The official Aura orientation note was
   written for the supported IMX415 setup and is not sufficient proof for an
   InnoMaker IMX296 assembly.
4. The factory MicroSD recovery image, writing tool, and a verified method to
   rewrite or replace the card are available.
5. Serial console or Ethernet/SSH access is available so a boot failure can be
   distinguished from a camera-driver failure.
6. The module-side power-enable, clock, reset, and auxiliary pins are resolved
   for the exact PCB revision. Published InnoMaker 15-pin material identifies
   pin 11 as Power Enable, pin 12 as LED, and pin 15 as 3.3 V, but does not yet
   resolve the required enable polarity or oscillator behavior for this board.

Do not connect XTR or make trigger-related board modifications for this
milestone. Raspberry Pi board-component references and trigger voltage
assumptions do not automatically apply to the InnoMaker module. In particular,
do not drive 54 MHz onto H3 IO1 based on another carrier's design.

## Phase 0 - acquire and freeze all vendor artifacts

Download from the official Aura pages:

- the latest Debian 13 **MicroSD** image;
- the matching Aura SDK archive;
- the official MicroSD-writing tool and formatter;
- the official Buildroot MicroSD image as an optional independent vendor
  diagnostic baseline.

Do not assume the documentation's example SDK name
`Luckfox_Aura_SDK_251224.tar.gz` is still the newest release. Record the actual
filenames, download dates, file sizes, and SHA-256 hashes:

```bash
sha256sum Luckfox_Aura_*_MicroSD_*.zip Luckfox_Aura_SDK_*.tar.gz \
  > vendor-downloads.sha256
```

Create an immutable bring-up ledger containing:

```text
Aura PCB revision
boot medium: MicroSD; card manufacturer, model, and capacity
camera module SKU and PCB revision
FFC type and orientation
vendor image filename and SHA-256
SDK archive filename and SHA-256
flashing-tool version
date flashed
```

### Preferred rollback arrangement

Keep the downloaded and hashed Debian 13 MicroSD archive and its extracted
`sd_update.img` as the immutable stock recovery baseline. A second known-good
MicroSD card is preferred before custom-firmware work, but the current 32 GB
card is sufficient for the initial stock baseline. Never flash an eMMC image to
this MicroSD-only unit.

## Phase 1 - flash and baseline the unmodified official image

### MicroSD procedure

Write the matching Debian 13 `sd_update.img` with balenaEtcher. Verify the
selected removable drive by capacity and device identity immediately before
writing, and retain the original image hash. Do not connect the IMX296 during
this stock-image flash or first boot.

### Stock-image acceptance

Boot without the IMX296 attached and collect:

```bash
mkdir -p ~/aura-bringup/stock
cd ~/aura-bringup/stock

cat /etc/os-release > os-release.txt
uname -a > uname.txt
id > identity.txt
lsblk -o NAME,PATH,SIZE,TYPE,FSTYPE,LABEL,PARTLABEL,MOUNTPOINTS,MODEL \
  > storage.txt
findmnt / > root-filesystem.txt
cat /proc/cmdline > cmdline.txt
tr -d '\0' </proc/device-tree/model > model.txt
tr '\0' '\n' </proc/device-tree/compatible > compatible.txt

if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz > kernel-config.txt
fi

for tool in dtc media-ctl v4l2-ctl i2cdetect; do
    command -v "$tool"
    "$tool" --version 2>&1 || true
done > tool-versions.txt 2>&1

sudo dtc -I fs -O dts \
  -o aura-stock-live.dts \
  /sys/firmware/devicetree/base

if [ -r /sys/firmware/fdt ]; then
    sudo cp /sys/firmware/fdt aura-stock-live.dtb
fi

v4l2-ctl --list-devices > v4l2-devices.txt 2>&1 || true
dmesg -T > dmesg.txt
```

Pass only if the board cold-boots repeatedly, Ethernet/SSH works, storage is
healthy, the complete UART boot log is saved, and the live device tree can be
extracted. `uname` alone does not identify the BSP source commit. If an IMX415 is available,
the official CSI0 test is a useful independent hardware baseline, but it is not
required to start the IMX296 port.

## Phase 2 - prove that the vendor SDK is reproducible

Use a clean Ubuntu 22.04 x86-64 system. Install the dependencies listed by
Luckfox, extract the SDK, and unpack its local repositories:

```bash
mkdir Aura-sdk
cd Aura-sdk
tar -xzvf ../Luckfox_Aura_SDK_<actual-version>.tar.gz
.repo/repo/repo sync -l
```

The `-l` option is required because the supplied archive contains local
repository content.

Before any edit, freeze the source identity. Keep the downloaded vendor image,
the unmodified SDK-produced image, and the eventual passing Nuvo image as three
separate named and hashed identities:

```bash
.repo/repo/repo manifest -r > manifest-vendor.xml
git -C sysdrv/source/kernel rev-parse HEAD > kernel-commit.txt
git -C sysdrv/source/kernel describe --always --dirty \
  > kernel-description.txt
make -s -C sysdrv/source/kernel kernelversion > kernel-version.txt

find sysdrv/source/kernel/arch/arm64/boot/dts/rockchip \
  \( -name 'rv1126b-luckfox-aura.dts' \
  -o -name 'rv1126b-luckfox-aura*.dtsi' \) \
  -print > aura-dts-files.txt

grep -Rns 'RK_KERNEL_DTS.*rv1126b-luckfox-aura' \
  project/cfg/BoardConfig_Aura \
  > selected-aura-dts.txt
```

Select the exact Aura/Debian/MicroSD configuration:

```bash
./build.sh lunch
```

Record the selected menu entry and the resulting board configuration. A copied
board configuration may not appear automatically in `lunch`; inspect the
target-discovery mechanism and explicitly register the new configuration if
required. Build an
**unmodified** full Debian image first:

```bash
sudo ./build.sh
sha256sum output/image/* > unmodified-sdk-image.sha256
```

Flash this unmodified SDK-produced image and repeat the Phase 1 checks. Stop if
it does not reproduce normal vendor-image behavior. A broken baseline SDK must
not be debugged at the same time as a new sensor driver.

## Phase 3 - inspect the exact Aura camera implementation

Before writing the IMX296 node, archive and inspect:

- `rv1126b-luckfox-aura.dts` and every included DTSI;
- the existing IMX415 node used for H3/CSI0;
- H3's I2C4 pinctrl group;
- the input-clock provider and configured frequency;
- reset, power-down, and power-enable GPIO assignments and active levels;
- regulator/fixed-supply nodes used by the completed camera module;
- the complete H3 endpoint chain from sensor to D-PHY, MIPI host, RKCIF, and
  optional RKISP virtual device;
- every `status` property and reciprocal `remote-endpoint`;
- the kernel configuration and the exact Rockchip media-driver APIs.

Also inspect the Rockchip host and bridge call sites before choosing a driver
baseline. Specifically determine whether this BSP consumes
`V4L2_CID_LINK_FREQ`, invokes `.get_mbus_config`, or relies on
`.g_frame_interval`; a driver that compiles without the interfaces expected by
the host can still fail at runtime. Record the inspected files and line numbers.

The public schematic supports H3/I2C4 and physical RX0, but the source DTS is the
authority for node labels, pinctrl, clock, GPIO, and media topology. Do not copy
GPIOs or endpoint names blindly from the Rockchip EVB dual-camera example.

## Phase 4 - create the single-camera firmware change

### 4.1 Driver change

Start with free-running operation only:

1. Add an IMX296 driver compatible with the actual Aura kernel. The public
   Rockchip `develop-6.1` camera Kconfig and Makefile do not currently expose
   `VIDEO_IMX296`, so a backport is expected.
2. Use the Linux v6.3 IMX296 driver as the provisional baseline only if the
   shipping Aura BSP proves to have a compatible 6.1-style V4L2 API. It is the
   first upstream version that contains the driver. Port it against the actual
   SDK headers and call sites rather than copying current Linux `master`.
3. Add `CONFIG_VIDEO_IMX296` to `drivers/media/i2c/Kconfig` and `imx296.o` to
   `drivers/media/i2c/Makefile`.
4. Enable it in `rv1126b_luckfox_defconfig`, initially built in (`=y`) to keep
   probe logs and deployment simple.
5. Preserve upstream chip identification, runtime power management, exposure,
   analogue gain, blanking, crop/format, and test-pattern controls.
6. Add a valid `V4L2_CID_LINK_FREQ` menu, `.get_mbus_config`, and
   `.g_frame_interval` if inspection proves the Rockchip receiver path requires
   them. Verify runtime behavior; successful compilation is not sufficient.
7. Preserve the upstream 2x2-binned 728 x 544 mode and verify that the port
   programs true sensor binning rather than a downstream crop or resize.
8. Do not add Raspberry Pi's XTR `trigger_mode` delta until normal free-running
   capture passes.

The upstream binding requires one input clock at 37.125, 54, or 74.25 MHz; AVDD
3.3 V, DVDD 1.2 V, OVDD 1.8 V; an optional XCLR reset; and one CSI-2 data lane.
For a completed camera module, DTS supply nodes must represent the real board/module
power arrangement rather than invented bare-sensor rails.

### 4.2 Single-camera device tree

Create a Nuvo-specific Aura single-camera DTS or DTSI with these invariants:

```text
sensor:        IMX296 at 0x1a
control bus:   H3 / I2C4
I2C pinctrl:   actual shipping label; public RV1126B reference suggests i2c4m2_pins
physical CSI:  H3 / RX0
data lanes:    one lane, matching both sensor and host endpoint
clock:         copied from verified H3 wiring and set to a supported rate
GPIOs:         copied only from the actual Aura H3 implementation
supplies:      matched to the completed module and actual Aura rails
media graph:   reciprocal sensor -> D-PHY -> CSI host -> RKCIF endpoints
CSI1/H4:       disabled and untouched for this milestone
XTR:           disabled; sensor is free-running
```

Disable the stock H3 IMX415 node rather than leaving two devices at different
addresses on the same physical endpoint. Use Rockchip camera-module metadata
only where required by the shipping BSP.

The native sensor format is the first target. Linux v6.3 already implements a
2x2-binned 728 x 544 mode, so preserve that mode while porting and prove from
the sensor format/crop state that it is genuine sensor binning. If native
capture works but 728 x 544 is absent or incorrect, the electrical and native
capture bring-up can be retained as a partial pass, but the single-camera
product milestone remains open.

### 4.3 Build and package

After the unmodified SDK build passes:

```bash
sudo ./build.sh clean kernel
sudo ./build.sh kernel
sudo ./build.sh firmware

sha256sum \
  output/image/sd_update.img \
  output/image/boot.img \
  > nuvo-imx296-single-image.sha256
```

Create a copied Aura Debian MicroSD board target and select the Nuvo DTS from
it. If the SDK uses an explicit target registry,
add the copied target there so it is selectable and reproducible; do not edit
the vendor target in place.

Archive with the image:

```text
manifest-vendor.xml
kernel commit and kernel version
kernel .config
original and patched DTS/DTSI
original and patched IMX296/Kconfig/Makefile files
compiler output
sd_update.img and boot.img hashes
```

## Phase 5 - flash the custom image

Use the same proven MicroSD-writing procedure from Phase 1. For the first
IMX296 build, write the complete SDK-produced `sd_update.img` to a MicroSD card;
do not introduce selective partition flashing as another variable.

Immediately after the flash:

1. Boot once with no camera attached and save the complete serial log.
2. Confirm the image identity, kernel version, and live DT match the build.
3. Power down completely.
4. Connect one camera to `CSI0`/H3 with the verified 22-to-15 cable orientation.
5. Leave H4 empty and leave XTR disconnected.
6. Power on and capture a second complete boot log.

## Phase 6 - single-camera validation ladder

Do not run the Nuvo daemon yet. Validate the V4L2 stack directly so failures are
localized to hardware, kernel, DTS, or media topology.

### Gate A - driver probe

```bash
uname -a
zcat /proc/config.gz | grep -E 'VIDEO_IMX296|MEDIA_CONTROLLER|VIDEO_ROCKCHIP' \
  || true

dmesg -T | grep -Ei \
  'imx296|i2c|camera|mipi|csi|dphy|rkcif|rkisp|regulator|clock|error|timeout'

find /sys/bus/i2c/devices -maxdepth 1 -name '*-001a' -print
```

Pass when the driver reads a valid sensor identity, binds exactly once on the
CSI0 I2C4 adapter, and reports no clock, regulator, reset, or I2C errors.

Use `i2cdetect` only while the sensor driver is unbound:

```bash
i2cdetect -l
i2cdetect -y <CSI0_I2C4_ADAPTER>
```

### Gate B - live DTS and complete media graph

```bash
sudo dtc -I fs -O dts \
  -o aura-imx296-single-live.dts \
  /sys/firmware/devicetree/base

for media_dev in /dev/media*; do
    echo "===== ${media_dev} ====="
    media-ctl -p -d "${media_dev}"
done | tee media-graphs.txt

v4l2-ctl --list-devices | tee v4l2-devices.txt
```

Pass when one IMX296 entity has a complete, enabled path through the expected
CSI0 physical D-PHY and CSI host to a usable RKCIF capture node, and every
enabled `remote-endpoint` is reciprocal.

### Gate C - enumerate formats and controls

Identify the raw RKCIF node from the media graph; do not assume `/dev/video13`
from the IMX415/ISP guide is the correct node.

```bash
v4l2-ctl -d /dev/videoX --all | tee imx296-all.txt
v4l2-ctl -d /dev/videoX --list-formats-ext | tee imx296-formats.txt
v4l2-ctl -d /dev/v4l-subdevX --list-ctrls-menus \
  | tee imx296-controls.txt
```

Record the actual Bayer order, bit depth, packing, stride, native dimensions,
frame intervals, exposure range, gain range, blanking controls, and test-pattern
support. `BG10` identifies the Bayer format but does not prove how RKCIF packs
samples in memory. Record the active RKCIF memory mode plus `bytesperline`,
`sizeimage`, and per-buffer `bytesused` before decoding a frame; distinguish
compact RAW10 from word-aligned storage.

### Gate D - first frames

Configure the sensor subdevice first, then the capture node. For the inspected
upstream timing model, frame rate is:

```text
74,250,000 / (1100 * (height + vertical_blanking))
```

This gives provisional `vertical_blanking=5662` for 1456 x 1088 at 10 Hz and
`vertical_blanking=6206` for 728 x 544 at 10 Hz. Recompute these values if the
ported driver's pixel rate, HMAX, or timing model differs. Do not use
`--set-parm=10` as the rate-control mechanism; the upstream IMX296 driver uses
the VBLANK control.

Use the exact sensor entity, pad, capture node, and enumerated RAW10 FourCC from
the media graph:

```bash
media-ctl -d /dev/mediaX \
  --set-v4l2 '"<IMX296_ENTITY>":0[fmt:SBGGR10_1X10/1456x1088]'

v4l2-ctl -d /dev/v4l-subdevX \
  --set-ctrl=vertical_blanking=5662

v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=1456,height=1088,pixelformat=<RAW10_FOURCC> \
  --stream-mmap=4 \
  --stream-count=8 \
  --stream-to=imx296-native-8frames.raw \
  --verbose 2>&1 | tee first-capture.log
```

Read back the subdevice control, capture format, `--get-parm` result where the
BSP implements it, and buffer metadata. Decode one frame offline using the
recorded memory mode, packing, stride, and Bayer order. Pass when
the image has correct geometry and stride, responds to light, has no repeated or
torn rows, and is not confused with an ISP/NV12 node.

### Gate E - controls

Exercise at least exposure, analogue gain, and test pattern if exposed. Capture
before and after each change. Pass when metadata/control reads agree and the
image changes in the expected direction without restarting the driver.

### Gate F - reliability

As a Nuvo project acceptance budget, run 50 stream-on/stream-off cycles, then a
600-frame capture. Retain timestamps for every dequeued buffer; 600 frames span
599 frame intervals, while command wall time also includes startup and teardown:

```bash
v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=<NATIVE_W>,height=<NATIVE_H>,pixelformat=<RAW10_FOURCC> \
  --stream-mmap=4 \
  --stream-count=600 \
  --stream-to=/dev/null \
  --verbose 2>&1 | tee native-600-frame.log

dmesg -T | grep -Ei \
  'imx296|csi|dphy|mipi|rkcif|ecc|crc|fifo|overflow|timeout|error' \
  | tee post-soak-dmesg.txt
```

Pass when the measured timestamp cadence satisfies the Nuvo-approved clock and
jitter budget around 10 Hz, no application drops are reported, and there are no
CSI CRC/ECC, RKCIF FIFO, frame-end timeout, I2C, or driver errors. Define the
allowed frequency error in ppm and the frame-interval jitter limit before using
this gate; do not substitute an arbitrary wall-clock range.

### Gate G - product mode

Configure the verified upstream sensor-binned mode and its provisional 10 Hz
VBLANK value, then run the same 600-frame test:

```bash
media-ctl -d /dev/mediaX \
  --set-v4l2 '"<IMX296_ENTITY>":0[fmt:SBGGR10_1X10/728x544]'

v4l2-ctl -d /dev/v4l-subdevX \
  --set-ctrl=vertical_blanking=6206

v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=728,height=544,pixelformat=<RAW10_FOURCC> \
  --stream-mmap=4 \
  --stream-count=600 \
  --stream-to=/dev/null \
  --verbose 2>&1 | tee product-mode-600-frame.log
```

Then run a 30-minute, 18,000-frame soak. These soak, restart, and 10/10 cold-boot
counts are Nuvo project acceptance budgets, not vendor guarantees. The
product-mode gate passes with deterministic probe, timestamp cadence inside the
approved budget, zero frame drops, zero CSI/RKCIF errors, and successful
exposure/gain operation.

If native RAW works but 728 x 544 does not enumerate or produces incorrect
geometry, retain the native-mode pass and open a separate IMX296 binning/crop
mode task. Do not rewrite the DTS to fake a sensor mode.

## Failure localization

| Symptom | Investigate first |
| --- | --- |
| No `0x1a` device | H3 cable orientation, 3.3 V, I2C4 pinctrl/pull-ups, IO0/IO1, input clock |
| `0x1a` responds but probe fails | driver/kernel API, chip ID, supplies, reset polarity, input-clock rate |
| Sensor probes but no video node | reciprocal endpoints, D-PHY/CSI host/RKCIF status and port labels |
| Stream times out immediately | input clock, reset/power sequence, lane number/order, D-PHY selection |
| CRC/ECC errors | cable integrity/orientation, lane polarity/order, D-PHY timing |
| Frames are shifted/corrupt | RKCIF memory mode, RAW10 compact/word packing, bytesperline, bytesused, crop dimensions, Bayer format |
| Native mode works; 728 x 544 absent | driver mode table, sensor binning/crop implementation—not board DTS |
| RAW works; NV12/color is poor | missing Rockchip ISP IQ/tuning—not a basic IMX296 capture failure |

## Definition of normal single-camera operation

The single-camera milestone is complete only when all of the following are
archived and reproducible:

- exact SDK manifest, kernel commit, kernel version, DTS/DTSI, DTB, kernel
  configuration, firmware hashes, camera SKU, and board revision;
- 10/10 cold boots with deterministic IMX296 probe on CSI0/I2C4;
- one complete sensor-to-RKCIF media graph with no dangling endpoint;
- visually correct native-resolution RAW10 frames;
- exposure and analogue-gain controls behave normally;
- 50 stream start/stop cycles without a hang or leak;
- 600 native frames at 10 Hz with zero drops and zero transport errors;
- 18,000 sensor-binned frames at 728 x 544 / 10 Hz with cadence inside the
  approved clock/jitter budget, zero drops, and zero transport errors;
- the exact full `sd_update.img` that produced the result is retained and
  hashed.

Raw capture is the acceptance authority. ISP color/NV12 requires an IMX296 IQ
profile and can follow without invalidating a successful sensor bring-up.

## Handoff to dual-camera work

Freeze the passing single-camera image before changing H4, I2C3, CSI1, or XTR.
The dual-camera phase may begin only from that frozen image and should proceed in
this order:

1. Verify/add CSI1 I2C3 pull-ups and resolve any DSI/I2C3 pinctrl conflict.
2. Add the second free-running IMX296 on H4/I2C3.
3. Prove two independent `0x1a` devices and two complete media graphs.
4. Prove simultaneous free-running native and 728 x 544 capture.
5. Add the shared 1.8 V XTR conditioning and driver trigger support last.
6. Only then add the Luckfox V4L2 capture backend to Nuvo Depth and run stereo
   pairing, calibration, rectification, and SGBM tests.

The existing `tools/pi` path is Picamera2-specific and is not the Aura bring-up
authority. Aura integration should use V4L2/media-controller capture while
preserving the existing ROS-free downstream stereo and result contracts.

## Evidence bundle to return after the first bench session

Package these files for review:

```text
vendor-downloads.sha256
manifest-vendor.xml
kernel-commit.txt
kernel-version.txt
kernel .config
original Aura DTS and included DTSI files
patched single-camera DTS/DTSI
sd_update.img and boot.img SHA-256 hashes
stock and custom live DTS/DTB
stock and custom boot logs
media-graphs.txt
v4l2-devices.txt
imx296-all.txt
imx296-formats.txt
imx296-controls.txt
first raw sample and capture log
600-frame and 18,000-frame logs
post-soak dmesg
camera/module/FFC/PCB identification photos
```

## Primary references

- [Luckfox Aura operating-system installation and flashing](https://wiki.luckfox.com/Luckfox-Aura/Getting-Started/)
- [Luckfox Aura SDK image compilation](https://wiki.luckfox.com/Luckfox-Aura/SDK-Image-Compilation/)
- [Luckfox Aura CSI0/IMX415 baseline](https://wiki.luckfox.com/Luckfox-Aura/Debian13/CSI/)
- [Linux v6.3 IMX296 driver](https://github.com/torvalds/linux/blob/v6.3/drivers/media/i2c/imx296.c)
- [Linux `sony,imx296` binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/media/i2c/sony,imx296.yaml)
- [Raspberry Pi IMX296 external-trigger change, deferred until dual bring-up](https://github.com/raspberrypi/linux/commit/e1b03ea9e84320e6bf36a1486abaebbceadd7fc7)
- [Oracle Aura stop-line report](../../Oracle/nuvo-luckfox-aura-stop-line-check-oracle-20260903.md)
- [Oracle CSI0/H3 single-IMX296 implementation preflight](luckfox-aura-imx296-single-camera-oracle-preflight-20260907.md)
