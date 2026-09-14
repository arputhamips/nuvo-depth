# Nuvo Depth: Aura CSI0/H3 single-IMX296 implementation preflight

**Assessment date: September 7, 2026**
**Scope: one existing Nuvo IMX296 module, free-running RAW capture only.**

## 1. Executive go/no-go and confidence

**GO for artifact acquisition, board identification, recovery preparation, and an unmodified vendor-image baseline. NO-GO for approving a camera-connected custom image until the shipping SDK and the actual module’s electrical interface are identified.**

No Aura firmware was built, flashed, or tested during this review. The actual shipping Aura DTS and kernel checkout remain unavailable for inspection. Consequently, this report does **not** present a fabricated, compile-ready Aura patch.

| Decision                                                                   | Status                              | Confidence and reason                                                                                                                                      |
| -------------------------------------------------------------------------- | ----------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Obtain the current official firmware, SDK, and Windows tools               | **GO — VERIFIED entry points**      | Official download and build pages are available; the cloud file listing is not inspectable here.                                                           |
| Select an exact “latest” Debian image or SDK filename                      | **BLOCKED — UNRESOLVED**            | Neither filename, release metadata, nor a matching image/SDK relationship could be recovered from the cloud index.                                         |
| Flash an identified, appropriate vendor image with the camera disconnected | **GO after storage identification** | Official Loader, full-image flashing, and MaskRom procedures are documented.                                                                               |
| Treat the previous H3 pin-number table as a wiring authority               | **NO-GO — VERIFIED discrepancy**    | The attached schematic numbers **H3 pin 1 as 3.3 V and pin 22 as ground**, not the reverse. Physical ribbon-contact numbering still requires verification. |
| Select and approve a final IMX296 driver baseline                          | **BLOCKED — UNRESOLVED**            | First establish the actual kernel APIs. A v6.3 upstream baseline is a reasonable candidate for a confirmed 6.1-style BSP, not an approved drop-in driver.  |
| Achieve 728 × 544 without inventing a sensor mode                          | **STRONG INFERENCE**                | Upstream IMX296 already implements a 2×2-binned 728 × 544 mode. Actual Aura operation remains untested.                                                    |
| Proceed to CSI1, XTR, calibration, or Nuvo integration                     | **NO-GO**                           | Requires the frozen single-camera evidence package and exit gate in §14.                                                                                   |

### Four corrections that materially change the implementation

**First: the prior H3 numbering does not match the schematic.** In the supplied primary schematic, H3’s symbol has `VCC_3V3` on pin **1**, `CSI0_I2C4_SDA` on **2**, `CSI0_I2C4_SCL` on **3**, and ground on **22**. This may reflect a different connector-numbering convention from a familiar Raspberry Pi cable diagram; it is not permission to reverse a cable by intuition. Verify signals by continuity.

**Primary evidence:** `LuckfoxTECH/luckfox-aura-docs`, commit `7264f6eab0562195b1d0c53969fff6cb7df360c9`, [`Hardware/Schematic/Luckfox-Aura.pdf`](https://github.com/LuckfoxTECH/luckfox-aura-docs/blob/7264f6eab0562195b1d0c53969fff6cb7df360c9/Hardware/Schematic/Luckfox-Aura.pdf), page 1, **MIPI CSI / H3**, R53–R110. I inspected the attached PDF directly; [this enlarged H3 crop](sandbox:/mnt/data/aura_preflight/research/aura-h3.png) makes the numbering readable.

**Second: 728 × 544 is already a sensor mode in the upstream implementation.** With the full-array crop selected, the driver supports 1456 × 1088 and 728 × 544 through sensor binning. Requesting 728 × 544 only on a capture node is not equivalent to selecting that sensor mode. **VERIFIED:** `torvalds/linux`, tag `v6.3`, [`drivers/media/i2c/imx296.c`, lines 621–706](https://github.com/torvalds/linux/blob/v6.3/drivers/media/i2c/imx296.c#L621-L706). ([GitHub][1])

**Third: an upstream driver that compiles can still fail Rockchip streaming.** The inspected Rockchip reference requires interfaces absent from the unmodified upstream IMX296 baseline: a **link-frequency menu control**, **`get_mbus_config`**, and, on the inspected RKCIF path, **`g_frame_interval`**. Whether the shipping Aura code has these exact requirements is **UNRESOLVED**, but these are concrete preflight checks—not speculative enhancements. **VERIFIED reference:** `rockchip-linux/kernel`, `develop-6.1`, [`phy-rockchip-csi2-dphy.c`, lines 78–108 and 420–448](https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/phy/rockchip/phy-rockchip-csi2-dphy.c#L78-L108), and [`cif/capture.c`, lines 7883–7900](https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/media/platform/rockchip/cif/capture.c#L7883-L7900). ([GitHub][2])

**Fourth: `BG10` does not, by itself, prove the memory packing on this vendor stack.** The inspected RKCIF implementation can enable compact RAW storage through a private memory-mode setting while retaining the nominal Bayer FourCC. Read the memory mode, stride, and buffer sizes before interpreting bytes. **VERIFIED reference:** `rockchip-linux/kernel`, `develop-6.1`, [`capture.c`, lines 9327–9377, 9442–9451, and 10389–10428](https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/media/platform/rockchip/cif/capture.c#L9327-L9377). ([GitHub][3])

---

## 2. Verified firmware, SDK, and tool selections

### Firmware and SDK

| Artifact                                                                    | Selection and evidence status                                                                                                                                                         |
| --------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Official Aura downloads page                                                | **VERIFIED:** [Luckfox Aura Downloads](https://wiki.luckfox.com/Luckfox-Aura/Downloads/).                                                                                             |
| Official cloud entry point                                                  | **VERIFIED:** [Luckfox Aura Google Drive folder](https://drive.google.com/drive/folders/1n5aovHVldUqts--or2wogNnBuv_bwVRR?usp=sharing), linked by both Downloads and Getting Started. |
| Exact current Debian 13 image filename and release date                     | **UNRESOLVED.** The cloud index returned no inspectable file listing.                                                                                                                 |
| Direct firmware-file URL and published firmware checksum                    | **UNRESOLVED.** Do not substitute the folder URL for a verified file URL.                                                                                                             |
| Separate eMMC and MicroSD distributions                                     | **VERIFIED at documentation level.** The installation page explicitly distinguishes the media. Exact current files remain unresolved.                                                 |
| Exact latest SDK filename, release date, direct URL, and published checksum | **UNRESOLVED.**                                                                                                                                                                       |
| `Luckfox_Aura_SDK_251224.tar.gz`                                            | **VERIFIED as a documentation example only. Not verified as the current SDK.** The wiki explicitly warns that the filename changes.                                                   |
| Latest firmware reproduced by latest SDK                                    | **UNRESOLVED.** “Latest” on both sides does not establish that they match. Obtain a vendor-confirmed pair or compare release manifests and outputs.                                   |

The cloud links were followed, but their file contents were not exposed to this research interface. The official SDK page itself directs the operator to obtain the current archive rather than copy its example filename. ([Luckfox][4])

**Exact user action to close this block:** open the official cloud folder in a desktop browser, locate the **Aura / Debian 13 / correct storage-medium** distribution and SDK, and preserve:

```text
Original archive filename
Exact file-sharing/download URL and cloud file ID
File size in bytes
Displayed modified/upload date
Release date stated in README/release notes, separately from upload date
Published checksum and its source, if present
Locally calculated SHA-256
Supported Aura SKUs / PCB revisions / boot media
The SDK revision or manifest explicitly associated with this image
```

If there is no release-to-SDK mapping in the package, ask Luckfox to identify the SDK archive and manifest used to build the downloaded image. Until then, the relationship remains **UNRESOLVED**, even if both downloads are official.

### Windows tools

**VERIFIED:** the official `LuckfoxTECH/luckfox-aura-docs` release [`v0.0.1`](https://github.com/LuckfoxTECH/luckfox-aura-docs/releases/tag/v0.0.1) offers these exact assets and SHA-256 digests. The asset timestamps below are **upload timestamps**, not necessarily the software’s original release dates. ([GitHub][5])

| Tool                                              | Exact official asset                                                                                                                           | Upload timestamp        |
| ------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------- |
| Rockchip USB driver installer                     | [`DriverAssitant_v5.13.zip`](https://github.com/LuckfoxTECH/luckfox-aura-docs/releases/download/v0.0.1/DriverAssitant_v5.13.zip)               | 2026-07-07 06:18:35 UTC |
| eMMC/full-image flashing                          | [`SocToolKit_V2.2.zip`](https://github.com/LuckfoxTECH/luckfox-aura-docs/releases/download/v0.0.1/SocToolKit_V2.2.zip)                         | 2026-07-07 06:18:46 UTC |
| MicroSD writing, when that is the selected medium | [`balenaEtcher-Portable-1.7.7.zip`](https://github.com/LuckfoxTECH/luckfox-aura-docs/releases/download/v0.0.1/balenaEtcher-Portable-1.7.7.zip) | 2026-07-07 06:17:50 UTC |

The `DriverAssitant` spelling is the actual asset filename.

```text
DriverAssitant_v5.13.zip
b2cabce5ab23551d8649c1201ef741fd73c8326044b78edac5439f013f034ff9

SocToolKit_V2.2.zip
92484ec262948a89e93a25f8a6de6c9560735b11b9ba4f96845aaa5c46fa7020

balenaEtcher-Portable-1.7.7.zip
bd4603f954c7db284514734651a0bd21a222624b843f9a123065077d65a96757
```

On Windows:

```powershell
Get-FileHash .\DriverAssitant_v5.13.zip -Algorithm SHA256
Get-FileHash .\SocToolKit_V2.2.zip -Algorithm SHA256
Get-FileHash .\balenaEtcher-Portable-1.7.7.zip -Algorithm SHA256

Get-ChildItem -File |
    Get-FileHash -Algorithm SHA256 |
    Export-Csv .\download-hashes.csv -NoTypeInformation
```

### Which storage medium is safest first?

**VERIFIED:** Aura `02000` and `04000` are the 2 GB and 4 GB variants without default eMMC; `02064` and `04064` are the corresponding 64 GB-eMMC variants. The board SKU must therefore be identified before choosing the image. Primary source: [Luckfox Aura Introduction, specifications](https://wiki.luckfox.com/Luckfox-Aura/Introduction/), lines 34–54. ([Luckfox][6])

**STRONG INFERENCE — recommended first baseline:**

* For an **eMMC-equipped board**, use the identified vendor **eMMC** image, with MicroSD removed, after protecting any existing data and establishing recovery.
* For a **no-eMMC board**, use the identified vendor **MicroSD** image. Do not follow an eMMC `update.img` flashing procedure against nonexistent storage.

**UNRESOLVED:** whether booting the current MicroSD distribution preserves an already populated eMMC. Writing a card in a PC cannot alter the disconnected board, but that says nothing about the board’s subsequent bootloader choice or an image’s initialization scripts.

To approve “MicroSD leaves eMMC untouched,” obtain the current boot-order implementation, inspect the SD image’s startup/update scripts, and record the UART boot path. Do not claim nondestructive fallback from the mere presence of a card slot.

No verified PCB-revision compatibility matrix was recovered. Match the actual board revision against the downloaded package’s notes or a vendor statement.

---

## 3. Remaining artifacts the user must obtain

| Blocked input                   | Required evidence                                                                      | Closure action                                                                  |
| ------------------------------- | -------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| Current firmware/SDK identities | Actual archives, release notes, file URLs, sizes, checksums                            | Complete the download record in §2.                                             |
| Shipping kernel identity        | Version, commit or source-package identity, local changes, configuration               | Run the extraction audit below.                                                 |
| Actual Aura board source        | `rv1126b-luckfox-aura.dts` and its complete include closure                            | Return the actual file—not an EVB or SoC substitute.                            |
| Exact Debian/storage target     | Selected board configuration and generated selection state                             | Preserve `lunch` output, configuration file, and build log.                     |
| Image/SDK relationship          | Vendor mapping or reproducibility comparison                                           | Ask Luckfox for the release manifest; compare rebuilt outputs.                  |
| Actual camera module            | Invoice/SKU, front/back photographs, PCB revision, connector identification            | Photograph the existing Nuvo unit; obtain the matching vendor manual/schematic. |
| Module clock and power behavior | Oscillator frequency, regulator/enable topology, reset behavior, I/O voltage tolerance | Obtain vendor documentation; verify accessible signals at the bench.            |
| Cable mapping                   | Signal-to-signal continuity record and contact orientation                             | Check the actual cable and connector, not a generic Pi drawing.                 |

### Known module context

**VERIFIED from the supplied Nuvo files:** `nuvo/docs/async-zero-copy-trigger-results.md`, lines 5–9, identifies **InnoMaker IMX296 color modules**. Lines 21–23 reference a **V2.0.2 manual**. This does not identify the exact PCB revision or electrical implementation.

The vendor’s [color-camera product page](https://www.inno-maker.com/product/cam-imx296color/) identifies the `CAM-IMX296Color` family, and its [IMX296 documentation](https://docs.inno-maker.com/home/mipi-cameras/imx296-mipi-cameras) publishes a 15-pin interface table. That is **family-level evidence**, not proof that every listed detail matches the particular Nuvo boards. ([Inno Maker][7])

Ask InnoMaker these specific questions:

> For our photographed module and PCB revision, identify the FFC connector pin numbering and mating view; input supply and I²C/GPIO voltage tolerances; CAM_IO0 polarity and exactly which circuitry it enables; CAM_IO1 function; oscillator frequency and enable behavior; onboard regulator sequencing; whether XCLR is exposed or handled onboard; whether the model register can be read; and the documented normal free-running power-up configuration.

Do not import the official Raspberry Pi Global Shutter Camera’s regulator, reset, or modification instructions as answers.

### Commands to establish the extracted SDK identity

Run these after preparing the host in §7. Keep evidence **outside** the SDK checkout.

```bash
export SDK=/work/Aura-sdk
export E=/work/aura-evidence/00-sdk-identity
mkdir -p "$E"
cd "$SDK"
set -euo pipefail

test -x .repo/repo/repo
.repo/repo/repo sync -l 2>&1 | tee "$E/repo-sync-local.log"

.repo/repo/repo manifest -r -o "$E/manifest-pinned.xml"

.repo/repo/repo forall -c '
    echo "===== PROJECT: $REPO_PROJECT PATH: $REPO_PATH ====="
    git rev-parse HEAD
    git status --short
    git remote -v
' > "$E/projects.txt"

cp -L .repo/manifest.xml "$E/manifest-selected.xml"

export K
K="$(realpath sysdrv/source/kernel)"
test -f "$K/Makefile"

{
    echo "KERNEL_PATH=$K"
    sed -n '1,12p' "$K/Makefile"
    make -s -C "$K" kernelversion
    git -C "$K" rev-parse --show-toplevel
    git -C "$K" rev-parse HEAD
    git -C "$K" branch --show-current
    git -C "$K" describe --always --dirty
    git -C "$K" remote -v
} > "$E/kernel-identity.txt" 2>&1

git -C "$K" status --short > "$E/kernel-status.txt"
git -C "$K" diff --binary > "$E/kernel-worktree.patch"

find "$K/arch" -type f -name 'rv1126b-luckfox-aura.dts' \
    -print | tee "$E/aura-dts-paths.txt"

find project/cfg -type f -iname '*Aura*Debian*.mk' \
    -print | tee "$E/debian-board-configs.txt"

grep -RnsE \
 'RK_KERNEL_DTS|RK_KERNEL_DEFCONFIG|RK_BOOT_MEDIUM|LF_TARGET_ROOTFS|RK_BOOTARGS_CMA_SIZE' \
 project/cfg/BoardConfig_Aura > "$E/aura-config-values.txt"
```

**VERIFIED command basis:** Luckfox’s [SDK compilation guide](https://wiki.luckfox.com/Luckfox-Aura/SDK-Image-Compilation/), especially local `repo sync -l`, board configurations, and build-script layout; its [kernel configuration guide](https://wiki.luckfox.com/Luckfox-Aura/Kernel-Configuration/) identifies the kernel location. **UNRESOLVED:** whether the current archive preserves Git metadata exactly as expected. A failure here is evidence to retain, not an invitation to invent a commit. ([Luckfox][8])

If the archive is stripped of Git metadata, record that explicitly and preserve source hashes. Obtain the commit/manifest relationship from Luckfox.

For the **one actual Aura DTS** returned by `find`, preserve its contents and all relevant includes:

```bash
# Set this only to the exact Aura file returned above.
read -r -p "Absolute path of actual rv1126b-luckfox-aura.dts: " AURA_DTS
test -f "$AURA_DTS"
test "$(basename "$AURA_DTS")" = rv1126b-luckfox-aura.dts

nl -ba "$AURA_DTS" > "$E/aura-dts-numbered.txt"

grep -nE \
 '#include|/include/|imx415|i2c4|pinctrl|remote-endpoint|csi|dphy|rkcif|rkisp' \
 "$AURA_DTS" > "$E/aura-dts-index.txt"

DTS_DIR="$(dirname "$AURA_DTS")"
tar -czf "$E/dts-source-closure.tgz" -C "$K" \
    "${DTS_DIR#"$K"/}" include/dt-bindings
```

Also preserve the generated DTB dependency file/build command after compilation. A directory archive is useful supporting evidence; the dependency record identifies the include chain actually used.

---

## 4. Corrected flashing and bring-up runbook

The operating order should be:

```text
Identify hardware and artifacts
    → flash vendor image, camera disconnected
    → verify boot and recovery
    → build and boot unmodified SDK image
    → inspect actual BSP APIs and shipping graph
    → create isolated Nuvo target
    → validate electrical mapping
    → flash custom image
    → prove native RAW
    → prove controls and restart stability
    → prove 728 × 544 / 10 Hz
    → freeze image and evidence
```

### Claim- and command-level audit of the attached plan

References below are to `plans/luckfox-aura-imx296-single-camera-bringup.md` in the supplied ZIP.

| Plan location / assumption                                   | Correction                                                                                                                                                                                                    |
| ------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Lines 29–36: H3/I²C and generic cable assumptions            | **VERIFIED correction:** I²C4 and the pull-ups are supported by the schematic. The earlier pin-number convention is not. A generic 22-to-15 cable is acceptable only after actual signal mapping is verified. |
| Line 41: use 128 MB CMA                                      | **VERIFIED correction:** `128M` is shown in a **Buildroot configuration example**, not established as the shipping Debian setting. Preserve the Debian target’s value initially.                              |
| Line 51: ribbon orientation                                  | **VERIFIED/UNRESOLVED:** Aura-side contacts toward Ethernet are documented. The camera-side orientation in the Aura page describes its supported camera, not necessarily the InnoMaker module.                |
| Lines 58 and 487: Pi-specific physical trigger modifications | **UNRESOLVED applicability; remove from this phase.** No Q2/R11 or trigger-voltage assumption should be transferred to the InnoMaker board.                                                                   |
| Lines 93–96: MicroSD automatically preserves eMMC            | **UNRESOLVED.** Current boot ordering and SD-image behavior must be established.                                                                                                                              |
| Lines 100–116: flashing                                      | **VERIFIED with clarification:** distinguish full-image `Firmware → Upgrade` from partition `Search Path → Download`, and distinguish eMMC `update.img` from SD `sd_update.img`.                              |
| First-boot identity commands                                 | Add root access, actual storage identification, a running-DTB capture, full UART log, kernel configuration, and tool versions. `uname` alone does not identify the BSP commit.                                |
| Line 149: `.repo/repo/repo sync -l`                          | **VERIFIED:** retain the local-only operation. Do not perform a floating network sync.                                                                                                                        |
| Lines 168 onward: source location                            | `sysdrv/source/kernel` is documented. The exact Aura DTS location and include tree still require extraction.                                                                                                  |
| Lines 194 and 276–279: build commands                        | **VERIFIED:** Debian builds use `sudo`; the documented incremental sequence is kernel clean, kernel build, then firmware packaging. Prove that packaging produced a fresh full image.                         |
| Line 226: infer shipping support from public `develop-6.1`   | **Invalid inference.** That branch is a reference, not the recovered Aura SDK.                                                                                                                                |
| Lines 241 onward: sensor supplies and clock                  | Separate **completed-module input power** from sensor-internal rails. Do not feed 1.2 V/1.8 V into guessed FFC pins or assume IO1 is an input clock.                                                          |
| Lines 267 and 441: 728 × 544 may require a new mode          | **VERIFIED correction:** the inspected upstream implementation already provides 2×2 binning.                                                                                                                  |
| Line 335: `i2cdetect -y 4`                                   | Use `i2cdetect -l` for adapter discovery. Avoid broad probing or forced raw transactions against a bound, runtime-managed sensor. Linux adapter number 4 is not itself proof of the physical I²C4 controller. |
| Line 379: RAW format                                         | Enumerate it. For the selected unflipped color baseline, expect `SBGGR10_1X10` and normally `BG10`; separately establish vendor memory packing.                                                               |
| Line 384: `--set-parm=10`                                    | Not the upstream IMX296 rate-control mechanism. Use VBLANK, and provide the getter expected by the actual Rockchip host.                                                                                      |
| Lines 403 and 419: frame-count timing and tolerance          | At 10 Hz, **600 timestamps span 599 intervals, nominally 59.9 seconds**. Wall-clock command duration includes startup. Replace arbitrary timing windows with an explicit clock/timestamp error budget.        |
| Line 437: restart/boot/soak counts                           | They can be adopted as **project acceptance budgets**, not represented as Sony/Luckfox requirements or statistical reliability proof.                                                                         |
| Pi capture helpers and Nuvo pipeline                         | Picamera2, Pi overlays, `rp1_cfe`, Pi GPIO trigger scripts, YUV420 luma, and the Nuvo daemon are outside this bring-up path.                                                                                  |

The corresponding primary references are the official [SDK guide](https://wiki.luckfox.com/Luckfox-Aura/SDK-Image-Compilation/), [installation guide](https://wiki.luckfox.com/Luckfox-Aura/Getting-Started/), [CSI guide](https://wiki.luckfox.com/Luckfox-Aura/Debian13/CSI/), upstream [IMX296 implementation](https://github.com/torvalds/linux/blob/v6.3/drivers/media/i2c/imx296.c), and the RKCIF sources discussed in §§5 and 10. ([Luckfox][8])

**VERIFIED from the attached Nuvo configuration:** the existing Pi profile’s `728 × 544 YUV420` output is processed output. It does not establish that the Pi’s sensor was operating in the same native binned mode proposed here. Preserve that distinction; optical equivalence is a later question.

---

## 5. Exact file-by-file kernel/DTS patch map

### 5.1 Driver baseline selection

**Final selection: UNRESOLVED until the actual Aura headers and host call sites are inspected.**

| Candidate                                                       | Identity, license, and disposition                                                                                                                                                                                                                                                                                                  |
| --------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Current upstream implementation                                 | `torvalds/linux`, `master`, `drivers/media/i2c/imx296.c`; inspected file history includes commit [`f687d3785d2b6e1cf960d71c0c72bc6472f2e8aa`](https://github.com/torvalds/linux/commit/f687d3785d2b6e1cf960d71c0c72bc6472f2e8aa). SPDX `GPL-2.0`. Use for current fixes and comparison, not blind copying into an older vendor API. |
| Closest initial upstream baseline for a confirmed 6.1-style BSP | `torvalds/linux`, exact tag [`v6.3`](https://github.com/torvalds/linux/blob/v6.3/drivers/media/i2c/imx296.c), same path and license. Introduction commit [`cb33db2b6ccfe`](https://github.com/torvalds/linux/commit/cb33db2b6ccfe). **Provisional preferred starting point**, subject to API comparison and relevant fixes.         |
| Raspberry Pi reference                                          | `raspberrypi/linux`, [`rpi-6.1.y`](https://github.com/raspberrypi/linux/blob/rpi-6.1.y/drivers/media/i2c/imx296.c), same path, SPDX `GPL-2.0`. Useful as a 6.1 API example, but includes Pi-specific behavior and trigger support. Do not import wholesale.                                                                         |
| Trigger change specifically excluded                            | `raspberrypi/linux`, commit [`e1b03ea9e84320e6bf36a1486abaebbceadd7fc7`](https://github.com/raspberrypi/linux/commit/e1b03ea9e84320e6bf36a1486abaebbceadd7fc7). Do not apply its `trigger_mode` behavior in this phase.                                                                                                             |
| Rockchip conventions                                            | `rockchip-linux/kernel`, `develop-6.1`, `drivers/media/i2c/imx415.c` and the CIF/D-PHY sources. Reference only until compared with the extracted Aura tree.                                                                                                                                                                         |
| InnoMaker repository                                            | [`INNO-MAKER/cam-imx296raw-trigger`](https://github.com/INNO-MAKER/cam-imx296raw-trigger). Primary module context and platform instructions; not a verified Aura BSP driver baseline.                                                                                                                                               |

The current upstream file uses newer interfaces including `v4l2_sensor_clk_get`, state-access helpers, and changed initialization/PM conventions. The older source has different probe/state initialization details. Kernel release numbers alone do not resolve these differences in a backported vendor tree. ([GitHub][9])

Before choosing, inspect:

```bash
grep -nE 'probe_new|remove' "$K/include/linux/i2c.h"

grep -RnsE \
 'v4l2_sensor_clk_get|v4l2_subdev_get_pad_format|v4l2_subdev_state_get_format|v4l2_subdev_init_finalize' \
 "$K/include/media"

grep -nE 'init_state|init_cfg|get_mbus_config|g_frame_interval' \
 "$K/include/media/v4l2-subdev.h"

grep -RnsE \
 'V4L2_CID_LINK_FREQ|get_mbus_config|g_frame_interval|RKMODULE_GET_' \
 "$K/drivers/phy/rockchip" \
 "$K/drivers/media/platform/rockchip/cif"

find "$K/drivers/media/i2c" -maxdepth 1 -iname '*imx296*' -print
```

Save those outputs. If the SDK already contains a vendor IMX296 driver, audit it before replacing it.

### 5.2 Files to change

`$K` below means the **actual extracted** `sysdrv/source/kernel`.

| File                                                                         | Proposed change                                                                                                                                                                   |
| ---------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `$K/drivers/media/i2c/imx296.c`                                              | Import or adapt the approved baseline; retain free-running initialization; add only required Rockchip-facing interfaces and module power handling.                                |
| `$K/drivers/media/i2c/Kconfig`                                               | Add one `VIDEO_IMX296` entry following the local framework’s dependencies; ensure regmap-I²C support is selected appropriately.                                                   |
| `$K/drivers/media/i2c/Makefile`                                              | Add `obj-$(CONFIG_VIDEO_IMX296) += imx296.o` if absent.                                                                                                                           |
| `$K/Documentation/devicetree/bindings/media/i2c/sony,imx296.yaml`            | Preserve the sensor binding; document any local Rockchip metadata extension. Do not silently claim an unchanged upstream schema accepts additional vendor properties.             |
| Actual stock defconfig                                                       | **Do not modify it.** Copy to `rv1126b_luckfox_nuvo_imx296_defconfig`; enable IMX296 and required subdevice/media support. Prefer built-in IMX296 for the first diagnostic image. |
| Actual `rv1126b-luckfox-aura.dts`                                            | **Do not destroy the stock target.** Copy it to `rv1126b-luckfox-aura-nuvo-imx296.dts`, retaining its actual includes and unrelated board configuration.                          |
| New sibling `rv1126b-luckfox-aura-nuvo-imx296.dtsi`                          | Put the single-camera graph, module power/clock representation, stock-camera disablement, and Nuvo overrides here.                                                                |
| Actual DTB build list, if used by the SDK                                    | Add the Nuvo DTB target only if the vendor build mechanism requires it.                                                                                                           |
| `project/cfg/BoardConfig_Aura/BoardConfig-EMMC-RK801-Luckfox_Aura-Debian.mk` | Copy to a Nuvo-specific configuration; change only the DTS/defconfig selectors initially. For a no-eMMC board, copy the corresponding `SD_CARD` Debian file instead.              |
| Diagnostic rootfs overlay/package list                                       | Add only required diagnostic utilities. Preserve these additions as a separate change from the unmodified-SDK baseline.                                                           |
| CIF/D-PHY/ISP implementation                                                 | **No initial patch planned.** First adapt the sensor to the verified host contract. A host change requires separate evidence.                                                     |
| Pi overlays, Picamera2, trigger scripts, Nuvo daemon                         | **No changes; not used in this phase.**                                                                                                                                           |

The two documented Debian configuration filenames and `RK_KERNEL_DTS`/`RK_KERNEL_DEFCONFIG` selectors are **VERIFIED in the wiki**, not yet verified inside the current archive. ([Luckfox][8])

### 5.3 Minimum driver adaptation checklist

**STRONG INFERENCE — implementation requirements, conditional on actual BSP inspection:**

**Core APIs and lifecycle.** Match the actual I²C probe/remove signatures, subdevice state access, initialization callbacks, asynchronous registration, and media-entity initialization. Preserve runtime-PM reference balancing. Audit `v4l2_subdev_init_finalize()` failure handling and subdevice cleanup rather than reproducing old error-path defects. Do not transfer a new-kernel PM simplification without checking the older PM semantics.

**Register access.** Preserve the baseline’s 16-bit register addressing, 8-bit regmap values, and multi-byte sensor-value packing. Do not replace it with an IMX415 register helper merely because both use I²C.

**Power and reset.** Keep sensor rail names and reset semantics consistent with the binding, but represent the **completed module** accurately. An optional `reset-gpios` means sensor **XCLR**, not “whichever cable pin looks like enable.”

**Clock.** The binding supports 37.125, 54, and 74.25 MHz `inck` clocks. Select the rate present on the actual module; a DT frequency does not manufacture a physical clock. **VERIFIED binding:** `torvalds/linux`, `v6.3`, [`sony,imx296.yaml`, lines 18–65](https://github.com/torvalds/linux/blob/v6.3/Documentation/devicetree/bindings/media/i2c/sony%2Cimx296.yaml#L18-L65). ([GitHub][10])

**Rockchip link-frequency control.** If the shipping D-PHY matches the reference, add a read-only **integer-menu** `V4L2_CID_LINK_FREQ`. The control’s value is the menu index; the menu entry is the frequency. A DT `link-frequencies` property alone does not satisfy the inspected host. The Pi overlay uses **594 MHz**, corresponding to the sensor driver’s nominal **1188 Mbit/s** lane rate; this is a supported starting estimate, not a measured exact serializer rate. **VERIFIED references:** [Pi overlay, lines 65–72](https://github.com/raspberrypi/linux/blob/rpi-6.1.y/arch/arm/boot/dts/overlays/imx296-overlay.dts#L65-L72), upstream driver lines 400–408, and Rockchip D-PHY lines 78–108. ([GitHub][11])

**Bus configuration.** Parse and validate the endpoint; return CSI-2 D-PHY configuration with **one data lane**, matching its lane map and clock mode. Implement the actual BSP’s `get_mbus_config` signature.

**Frame-interval getter.** If the shipping RKCIF matches the reference, implement `g_frame_interval` from the **current active mode and VBLANK**, not an IMX415 constant. For the inspected baseline’s timing:

```text
interval numerator   = 1100 × (active_height + vertical_blanking)
interval denominator = 74,250,000
```

Use the correct locking and integer widths. Reduce the fraction where appropriate. A setter or frame-interval enumeration is not necessary merely to expose the actual interval; VBLANK remains the rate control.

**Rockchip metadata/private calls.** Inspect callers before adding private ioctls. The reference IMX415 reads module index, facing, module name, and lens name; implement meaningful metadata and `GET_MODULE_INFO` if the actual stack requires it. Do not return success for unimplemented private operations. The reference CIF already has defaults for some missing HDR/CSI information, so copying an entire Rockchip sensor ioctl switch is not justified. **VERIFIED reference:** `rockchip-linux/kernel`, `develop-6.1`, [`imx415.c`, lines 3046–3056](https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/media/i2c/imx415.c#L3046-L3056). ([GitHub][12])

### 5.4 Model detection, modes, formats, and controls

**VERIFIED:** the binding allows generic `sony,imx296` autodetection or explicit color `sony,imx296lq` / monochrome `sony,imx296ll`. Some modules prevent model-register reads. **UNRESOLVED:** which behavior applies to the exact Nuvo PCB. An explicit color compatible is valid when the module identity is established, but it must not be used to disguise a broken I²C/power path. ([GitHub][10])

For the proposed unflipped upstream-derived baseline:

| Property                                   | Evidence-backed expectation                                                                                                                                                                                  |
| ------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Sensor I²C address                         | `0x1a`.                                                                                                                                                                                                      |
| Enumerated full-array modes                | 1456 × 1088 and 728 × 544.                                                                                                                                                                                   |
| 728 × 544 mechanism                        | Sensor 2×2 binning when using the full-array crop.                                                                                                                                                           |
| Crop interaction                           | ROI cropping and binning are not freely interchangeable in this implementation.                                                                                                                              |
| Color media-bus code                       | `MEDIA_BUS_FMT_SBGGR10_1X10`.                                                                                                                                                                                |
| Nominal unpacked capture FourCC            | `V4L2_PIX_FMT_SBGGR10`, displayed as `BG10`; confirm actual RKCIF enumeration and memory mode.                                                                                                               |
| Upstream frame-interval enumeration/setter | Not provided by the inspected baseline. Do not invent a list of enumerated frame rates.                                                                                                                      |
| Controls                                   | Exposure in sensor timing units, integer analogue gain, VBLANK, read-only HBLANK/pixel rate, and test-pattern menu. Pi exposure microseconds and gain multipliers are not interchangeable with these values. |

**VERIFIED sources:** upstream [mode/control implementation](https://github.com/torvalds/linux/blob/v6.3/drivers/media/i2c/imx296.c), and `torvalds/linux`, `v6.3`, [`include/uapi/linux/videodev2.h`, lines 629–642](https://github.com/torvalds/linux/blob/v6.3/include/uapi/linux/videodev2.h#L629-L642). ([GitHub][1])

---

## 6. Evidence-supported Aura DTS outline

### 6.1 What the schematic actually establishes

**VERIFIED — schematic symbol numbering, not yet a mating-view cable drawing:**

| H3 schematic pin         | Signal               | Schematic evidence                                    |
| ------------------------ | -------------------- | ----------------------------------------------------- |
| 1                        | `VCC_3V3`            | H3 power connection                                   |
| 2                        | I²C4 SDA             | GPIO4_A6 → R98, 22 Ω → `CSI0_I2C4_SDA`                |
| 3                        | I²C4 SCL             | GPIO4_A7 → R100, 22 Ω → `CSI0_I2C4_SCL`               |
| 5                        | `CSI0_IO0`           | GPIO4_A0 → R102, 22 Ω                                 |
| 6                        | `CSI0_IO1`           | GPIO4_B0 → R106, 22 Ω                                 |
| 14 / 15                  | CSI0 clock P / N     | Physical RX0 clock0 through R89/R92, 2.2 Ω            |
| 20 / 21                  | CSI0 data0 P / N     | Physical RX0 D0 through R53/R56, 2.2 Ω                |
| 17 / 18                  | CSI0 data1 P / N     | Present on connector; not a second active IMX296 lane |
| 4, 7, 10, 13, 16, 19, 22 | Ground               | H3 ground net                                         |
| Pull-ups                 | SDA and SCL to 3.3 V | R109/R110, 4.7 kΩ                                     |

Source: the pinned schematic cited in §1, page 1, H3 and R53–R110.

**VERIFIED reference pinmux:** the public RV1126B pinctrl file assigns I²C4 mode 2 to **GPIO4_A7 SCL and GPIO4_A6 SDA**, matching these schematic nets. Its label is `i2c4m2_pins`, **not** `i2c4m0_pins`. **UNRESOLVED:** whether the shipping include tree uses that exact label. Primary source: `rockchip-linux/kernel`, `develop-6.1`, [`rv1126b-pinctrl.dtsi`, lines 645–670](https://github.com/rockchip-linux/kernel/blob/develop-6.1/arch/arm64/boot/dts/rockchip/rv1126b-pinctrl.dtsi#L645-L670). ([GitHub][13])

### 6.2 Module-side mapping

**VERIFIED at vendor-documentation level:** InnoMaker’s 15-pin table calls pin 11 **Power Enable**, pin 12 **LED Indicator**, and pin 15 **3.3 V Power Input**. It does not establish enable polarity or oscillator/reset implementation for the actual PCB. [Vendor pin table, lines 24–41](https://docs.inno-maker.com/home/mipi-cameras/imx296-mipi-cameras). ([Inno Maker Docs][14])

If the actual module is confirmed to use that table, these are the signal-continuity expectations:

| H3 schematic pin(s) | Vendor 15-pin module pin(s)                          |
| ------------------- | ---------------------------------------------------- |
| 1, 2, 3             | 15 power, 14 SDA, 13 SCL                             |
| 5, 6                | 11 power-enable, 12 LED                              |
| 20, 21              | 3 D0_P, 2 D0_N                                       |
| 14, 15              | 9 CLK_P, 8 CLK_N                                     |
| 17, 18              | 6 D1_P, 5 D1_N; unused by the one-lane sensor stream |
| 22, 19, 16, 13      | 1, 4, 7, 10 grounds, respectively                    |

This is a **STRONG INFERENCE mapping to test**, not a cable purchase specification or an approved mating-view drawing.

**Do not drive 54 MHz onto H3 IO1 merely because another board’s camera uses an external clock pin.**

### 6.3 Shipping graph and power representation

**UNRESOLVED:** the actual CSI0 IMX415 node, D-PHY labels, separate CSI host nodes, RKCIF/IOMMU labels, endpoints, and ISP attachment. The exact stock graph must be extracted from the Aura file and its includes.

The functional path to establish is:

```text
IMX296 on physical H3
    → physical MIPI RX0, clock0/data0
    → the BSP's appropriate logical D-PHY instance
    → the BSP's CSI-2 receiver path
    → RKCIF RAW capture
    → memory
```

Do not assume the logical D-PHY name is determined by the physical “RX0” name. The inspected RV1126B reference code distinguishes full-lane and split-lane attachment. Its one-lane selection must be traced against the actual shipping graph. **VERIFIED reference:** `rockchip-linux/kernel`, `develop-6.1`, [`phy-rockchip-csi2-dphy.c`, RV1126B attachment logic around lines 201–244](https://github.com/rockchip-linux/kernel/blob/develop-6.1/drivers/phy/rockchip/phy-rockchip-csi2-dphy.c#L201-L244). ([GitHub][2])

For power, represent the module’s actual onboard rails and common enable dependency. The FFC supplies one module input; it does not expose three independently supplied bare-sensor rails. Do not connect all three sensor supply phandles to a fictitious 3.3 V regulator simply to suppress probe errors.

### 6.4 Strict outline—not compile-ready DTS

All references beginning `TODO_` require evidence from the actual SDK/module. Existing endpoint overrides below assume those endpoints have labels; otherwise use their verified full node paths.

```dts
/*
 * Nuvo single-camera outline only.
 * NOT BUILDABLE until every TODO is resolved from the actual Aura BSP
 * and the actual InnoMaker module.
 */

/* Disable the actual stock sensor attached to H3. */
&TODO_STOCK_H3_IMX415 {
    status = "disabled";
};

/*
 * Define module clock/regulators separately after obtaining the
 * module circuit and power-enable behavior.
 *
 * Do not invent:
 *   - a CRU clock connection that is not physically wired;
 *   - a 54 MHz output on IO1;
 *   - reset-gpios pointing at a module power-enable input;
 *   - three external sensor supplies on the FFC.
 */

&TODO_ACTUAL_I2C4_NODE {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&TODO_VERIFIED_I2C4_MODE2_PINS>;

    #address-cells = <1>;
    #size-cells = <0>;

    nuvo_imx296: camera@1a {
        compatible = "sony,imx296";
        /*
         * TODO: confirm model-register readability.
         * Use "sony,imx296lq" only with established color-module identity
         * when generic autodetection is inappropriate.
         */
        reg = <0x1a>;
        status = "okay";

        clocks = <&TODO_ACTUAL_MODULE_INCK_PROVIDER>;
        clock-names = "inck";

        avdd-supply = <&TODO_ACTUAL_MODULE_AVDD>;
        dvdd-supply = <&TODO_ACTUAL_MODULE_DVDD>;
        ovdd-supply = <&TODO_ACTUAL_MODULE_OVDD>;

        /*
         * No guessed reset-gpios or pwdn-gpios.
         * IO0 ownership/polarity belongs in the verified module
         * power representation.
         */

        rockchip,camera-module-index = <0>;
        rockchip,camera-module-facing = "TODO_CONFIRMED_FACING";
        rockchip,camera-module-name = "TODO_ACTUAL_MODULE_SKU";
        rockchip,camera-module-lens-name = "TODO_ACTUAL_LENS";

        port {
            nuvo_imx296_out: endpoint {
                remote-endpoint = <&TODO_H3_DPHY_INPUT_ENDPOINT>;
                clock-lanes = <0>;
                data-lanes = <1>;

                /*
                 * TODO: confirm clock mode and link-frequency policy.
                 * Pi reference: noncontinuous clock, nominal 594 MHz.
                 * DT data must agree with the sensor's host-facing
                 * bus configuration and link-frequency control.
                 */
            };
        };
    };
};

&TODO_H3_PHYSICAL_DPHY_HARDWARE {
    status = "okay";
};

&TODO_CORRECT_ONE_LANE_LOGICAL_DPHY {
    status = "okay";
};

&TODO_H3_DPHY_INPUT_ENDPOINT {
    remote-endpoint = <&nuvo_imx296_out>;
    clock-lanes = <0>;
    data-lanes = <1>;
};

&TODO_H3_DPHY_OUTPUT_ENDPOINT {
    remote-endpoint = <&TODO_CSI_HOST_INPUT_ENDPOINT>;
};

&TODO_CSI_HOST_INPUT_ENDPOINT {
    remote-endpoint = <&TODO_H3_DPHY_OUTPUT_ENDPOINT>;
};

&TODO_CSI_HOST {
    status = "okay";
};

&TODO_CSI_HOST_OUTPUT_ENDPOINT {
    remote-endpoint = <&TODO_RKCIF_INPUT_ENDPOINT>;
};

&TODO_RKCIF_INPUT_ENDPOINT {
    remote-endpoint = <&TODO_CSI_HOST_OUTPUT_ENDPOINT>;
};

&TODO_RKCIF_INSTANCE {
    status = "okay";
};

&TODO_REQUIRED_RKCIF_IOMMU {
    status = "okay";
};

/*
 * Disable unused CSI1 sensor/path and the H3→ISP attachment using their
 * actual nodes. Do not indiscriminately disable shared hardware.
 * No XTR GPIO, trigger mode, second sensor, or synchronization properties.
 */
```

**STRONG INFERENCE:** a correct RKCIF-to-memory RAW path avoids making RKAIQ tuning a prerequisite. An absent IMX296 IQ file can block an ISP/color demonstration without invalidating sensor RAW operation. The actual shipping path still needs confirmation.

---

## 7. Build and packaging commands for the Aura Debian target

### 7.1 Host preparation

**VERIFIED:** Luckfox specifies Ubuntu 22.04 x86-64 and distinguishes Debian’s root-requiring live-build flow from Buildroot’s normal-user flow. Use a dedicated VM or dedicated host snapshot. Do not change a general workstation’s default Python as an incidental setup step. [Official SDK instructions](https://wiki.luckfox.com/Luckfox-Aura/SDK-Image-Compilation/), host setup and Debian compilation sections. ([Luckfox][8])

The documented dependency set is:

```bash
sudo apt-get update
sudo apt-get install -y \
  git ssh make gcc libssl-dev liblz4-tool expect expect-dev g++ \
  patchelf chrpath gawk texinfo diffstat binfmt-support qemu-user-static \
  live-build bison flex fakeroot cmake gcc-multilib g++-multilib unzip \
  device-tree-compiler ncurses-dev libgucharmap-2-90-dev bzip2 expat \
  gpgv2 cpp-aarch64-linux-gnu libgmp-dev libmpc-dev bc \
  python-is-python3 python2 curl file rsync bsdmainutils dpkg-dev po4a gettext
```

The wiki currently directs `python` to Python 2. Apply that only in the dedicated environment when following the selected archive’s requirements:

```bash
python --version
command -v python2

# Dedicated Aura build VM only:
sudo ln -sfn /usr/bin/python2 /usr/bin/python
```

**VERIFIED known build issue:** Luckfox’s engineer confirms that an old live-build can lack Debian `trixie` support. Inspect the extracted `sysdrv/tools/board/debian/check-debian.sh`; if it requests an upgrade, follow that archive’s requirement and pin the resulting live-build source commit. Do not assume Ubuntu’s initial package is sufficient or clone an arbitrary moving HEAD. [Official support thread, engineer response dated February 3, 2026](https://forums.luckfox.com/viewtopic.php?t=2377). ([Luckfox Forums][15])

Capture the build environment:

```bash
mkdir -p /work/aura-evidence/host
uname -a > /work/aura-evidence/host/uname.txt
lsb_release -a > /work/aura-evidence/host/os.txt 2>&1
dpkg-query -W > /work/aura-evidence/host/packages.tsv
python --version > /work/aura-evidence/host/python.txt 2>&1
lb --version > /work/aura-evidence/host/live-build.txt 2>&1
```

### 7.2 Unmodified SDK build first

Extract the **actual downloaded SDK archive**, retaining its original filename and hash. Then perform the identity collection in §3.

For an identified eMMC board, the documented target to locate is:

```text
project/cfg/BoardConfig_Aura/
  BoardConfig-EMMC-RK801-Luckfox_Aura-Debian.mk
```

For a no-eMMC board:

```text
project/cfg/BoardConfig_Aura/
  BoardConfig-SD_CARD-RK801-Luckfox_Aura-Debian.mk
```

Do not hard-code a `lunch` menu number from another release.

```bash
cd "$SDK"
set -euo pipefail

mkdir -p /work/aura-evidence/01-stock-sdk-build

script -q -c './build.sh lunch' \
  /work/aura-evidence/01-stock-sdk-build/lunch.typescript

# Select the actual Aura + Debian + correct boot-medium entry.

sudo ./build.sh 2>&1 |
  tee /work/aura-evidence/01-stock-sdk-build/full-build.log

find output -maxdepth 3 -type f \
  \( -name '*.img' -o -name '*.bin' -o -name '*package*' \) \
  -printf '%p %s bytes\n' |
  tee /work/aura-evidence/01-stock-sdk-build/artifacts.txt
```

**VERIFIED:** these are the documented Debian build commands. The official flashing documentation identifies `output/image/update.img` as the full-image output. **UNRESOLVED:** whether the current downloaded SDK successfully generates it without additional release-specific fixes. ([Luckfox][8])

Flash and validate this unmodified SDK image before changing camera support.

**Do not equate a successful build with reproduction of the downloadable image.** Compare boot behavior, kernel configuration, DTB, partition layout, rootfs package manifest, and any vendor camera baseline available. Byte-for-byte equality is a separate question; changing apt inputs and build timestamps can prevent it.

### 7.3 Create the isolated Nuvo target

Use a second preserved SDK workspace or otherwise retain an untouched stock build.

For the eMMC branch, after confirming these files exist:

```bash
cd "$SDK"

STOCK_CFG=project/cfg/BoardConfig_Aura/BoardConfig-EMMC-RK801-Luckfox_Aura-Debian.mk
NUVO_CFG=project/cfg/BoardConfig_Aura/BoardConfig-EMMC-RK801-Luckfox_Aura-Nuvo-IMX296-Debian.mk

test -f "$STOCK_CFG"
cp -a "$STOCK_CFG" "$NUVO_CFG"

# Confirm the actual stock DTS and defconfig paths first.
test -f "$AURA_DTS"
test -f "$K/arch/arm64/configs/rv1126b_luckfox_defconfig"

cp -a "$AURA_DTS" \
  "$(dirname "$AURA_DTS")/rv1126b-luckfox-aura-nuvo-imx296.dts"

cp -a "$K/arch/arm64/configs/rv1126b_luckfox_defconfig" \
  "$K/arch/arm64/configs/rv1126b_luckfox_nuvo_imx296_defconfig"
```

Change the copied configuration’s selectors to:

```make
export RK_KERNEL_DTS=rv1126b-luckfox-aura-nuvo-imx296.dts
export RK_KERNEL_DEFCONFIG=rv1126b_luckfox_nuvo_imx296_defconfig
```

Preserve its actual Debian rootfs, RK801, storage, partition, and boot-argument settings. Complete the source changes in §§5–6. Add the Nuvo DTSI include to the copied DTS.

**UNRESOLVED:** whether the current `lunch` implementation automatically discovers the copied filename. Inspect its configuration-discovery code; if it uses an explicit list, add the new target there. Do not claim a new menu entry exists before verifying it.

### 7.4 Build and package the custom full image

```bash
cd "$SDK"
set -euo pipefail

export BUILD_E=/work/aura-evidence/02-nuvo-build
mkdir -p "$BUILD_E"
touch "$BUILD_E/build-start"

script -q -c './build.sh lunch' "$BUILD_E/lunch.typescript"
# Select the verified Nuvo + Debian + correct-medium target.

# First custom image: full build.
sudo ./build.sh 2>&1 | tee "$BUILD_E/full-build.log"

# Subsequent kernel/DTS iterations, after the full build works:
sudo ./build.sh clean kernel 2>&1 | tee "$BUILD_E/kernel-clean.log"
sudo ./build.sh kernel       2>&1 | tee "$BUILD_E/kernel-build.log"
sudo ./build.sh firmware     2>&1 | tee "$BUILD_E/package.log"
```

For the eMMC branch:

```bash
test -s output/image/update.img

stat output/image/update.img | tee "$BUILD_E/update-stat.txt"
file output/image/update.img | tee "$BUILD_E/update-file.txt"
sha256sum output/image/update.img | tee "$BUILD_E/update.sha256"

find output/image -maxdepth 1 -type f -print0 |
  sort -z |
  xargs -0 sha256sum > "$BUILD_E/image-components.sha256"
```

A recent timestamp alone is not sufficient. Inspect the actual packaging script and package manifest to establish that the full image includes the newly built kernel/DTB and matching modules/rootfs:

```bash
grep -RnsE 'update\.img|sd_update\.img|package-file|afptool|rkImageMaker' \
  project tools sysdrv > "$BUILD_E/packaging-code-index.txt"
```

Use the unpacking tool and syntax shipped with that SDK, after inspecting its help/script invocation. **UNRESOLVED:** its exact executable path and command line in the current archive.

For Windows full-image eMMC flashing, the artifact is **`update.img`**, not `Image`, a standalone DTB, `boot.img`, or `download.bin`. The latter is documented as the device-side flashing communication program. For MicroSD, use the actual packaged **`sd_update.img`** path established by that target; do not raw-write the Rockchip `update.img` container to a card. ([Luckfox][8])

---

## 8. Windows flashing and recovery procedure

### Normal Loader/full-image route

**VERIFIED procedure:** [Luckfox installation guide, §§1–5](https://wiki.luckfox.com/Luckfox-Aura/Getting-Started/). ([Luckfox][16])

1. Leave both cameras disconnected. Remove MicroSD for the eMMC baseline. Protect any existing eMMC data; a full upgrade is not a nondestructive operation.
2. Install **DriverAssistant 5.13**, then restart Windows as directed.
3. Connect the board’s **USB data/OTG port** to Windows and provide the separate, stable **5 V / 3 A power supply** specified by Luckfox.
4. Start **SocToolKit 2.2 as administrator**.
5. Hold **RESET**, then hold **BOOT**. Release **RESET** while continuing to hold BOOT. Release BOOT only after the tool recognizes Loader mode.
6. Use the **full-image** route: **Firmware → select the verified full image → Upgrade**.
7. Preserve the selected path, SHA-256, tool log, success indication, and first UART boot log.

Do not mix this with the independent partition-flashing route, `Search Path → Download`.

When a working system is reachable, the official guide also documents `reboot loader` as a software entry to flashing mode. Keep the physical-button method available for a broken boot.

### MaskRom recovery

**VERIFIED documented fallback:** the same guide’s §4.2 instructs connecting USB data, removing board power, shorting the indicated pair of pads, and reapplying power until `Found One MASKROM Device` appears.

Use the [exact official MaskRom-pad photograph](https://wiki.luckfox.com/assets/images/maskrom-18611ff5ad8ed2ec88715d94dfe8d1ae.webp). I inspected that image. No readable pad designators were established, so this report does not invent names for them. Match the photo against the actual PCB revision before touching the board. ([Luckfox][17])

Remove the short after recognition and restore the known-good full vendor image using the correct RV1126B package. Do not use a loader from a Pico/RV1106 example or start by erasing all flash.

**STRONG INFERENCE — recovery acceptance:** demonstrate recovery to the vendor baseline before relying on it for camera development. The archived image, its hash, the correct tools, and a successful recovery log are the rollback package.

---

## 9. First-boot and CSI0 connection sequence

### 9.1 Boot with no camera first

**VERIFIED:** Luckfox specifies a default UART baud rate of **1,500,000**. Use an adapter that supports it and verify the board’s actual UART pinout/electrical levels before connection. Do not attach an RS-232-level interface or use an adapter’s supply pin as an unverified power source. ([Luckfox][16])

Capture a complete power-on log. On the running board, collect:

```bash
sudo -i
mkdir -p /root/aura-evidence/boot
E=/root/aura-evidence/boot

uname -a > "$E/uname.txt"
cat /proc/version > "$E/proc-version.txt"
cat /etc/os-release > "$E/os-release.txt"
cat /proc/cmdline > "$E/cmdline.txt"

tr '\0' '\n' < /proc/device-tree/model > "$E/model.txt"
tr '\0' '\n' < /proc/device-tree/compatible > "$E/compatible.txt"

findmnt -no SOURCE,FSTYPE,OPTIONS / > "$E/root-mount.txt"
lsblk -o NAME,PATH,SIZE,TYPE,FSTYPE,MOUNTPOINTS > "$E/lsblk.txt"

for p in /sys/block/mmcblk*/device; do
    [ -d "$p" ] || continue
    echo "=== $p ==="
    for f in type name cid; do
        [ ! -r "$p/$f" ] || { echo "$f:"; cat "$p/$f"; }
    done
done > "$E/mmc-identities.txt"

dmesg > "$E/dmesg.txt"
journalctl -b --no-pager > "$E/journal.txt"

if [ -r /sys/firmware/fdt ]; then
    cp /sys/firmware/fdt "$E/running.dtb"
fi
```

After installing `device-tree-compiler` in the **diagnostic** environment:

```bash
dtc -I fs -O dts /sys/firmware/devicetree/base \
    > "$E/running.dts" 2> "$E/dtc-warnings.txt"

if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz > "$E/running.config"
fi
```

Keep the untouched vendor-image identity separate from later diagnostic package installations.

### 9.2 Electrical authorization before attaching the module

**STRONG INFERENCE — required bench checks:**

Disconnect all possible power feeds, including USB power/data backfeed and PoE where applicable. Verify the camera supply is discharged before continuity testing.

Confirm the actual cable’s power, grounds, clock pair, D0 pair, SDA, SCL, and IO connections against §6. Check the physical contact orientation at **both** ends. Confirm the module accepts the H3 pull-up voltage and does not have an incompatible enable/reset interface.

Establish the oscillator frequency and module-enable behavior from documentation and, where accessible, measurement. A successful `clk_get_rate()` only proves what software was told.

Boot the **custom image with the camera still disconnected** first and check for unintended GPIO/pinmux assignments. In particular, IO1 must not be driven as a guessed clock.

### 9.3 Attach one camera

Power down completely. Connect only the confirmed module to **CSI0/H3**, not the similarly shaped DSI connector. Leave H4/CSI1 empty. Leave external-trigger wiring disconnected and do not apply trigger-related board modifications.

**VERIFIED Aura-side instruction:** ribbon metal contacts face toward Ethernet. **UNRESOLVED actual module-side instruction:** establish it from the real connector and vendor documentation, not the IMX415 photograph. ([Luckfox][18])

Cold-power the module so it begins from its normal free-running state.

---

## 10. Exact V4L2/media-controller validation commands

### Diagnostic helpers

The companion [bench-tools source ZIP](sandbox:/mnt/data/aura-single-camera-bench-tools-20260907.zip) contains:

* `cif_memory_mode.c`: queries the actual vendor memory-mode ioctl and can request low-aligned word storage.
* `check_frames.py`: checks verbose dequeue logs, sequence continuity, timestamps, error flags, and buffer lengths.
* `inspect_word_raw.py`: inspects already-confirmed low-aligned word RAW and writes a fixed-scale monochrome Bayer-mosaic preview.

These are **diagnostic source helpers, not an Aura driver or firmware patch**. Fifteen local synthetic/syntax tests passed. Neither the missing Aura SDK nor Aura hardware was used to validate them.

### 10.1 Install tools and release camera ownership

In the diagnostic image:

```bash
sudo apt-get update
sudo apt-get install -y \
    v4l-utils device-tree-compiler i2c-tools psmisc \
    python3 python3-numpy python3-pil build-essential
```

Run the following as root:

```bash
mkdir -p /root/aura-evidence/camera
export E=/root/aura-evidence/camera
set -o pipefail

v4l2-ctl --version | tee "$E/v4l-utils-version.txt"
v4l2-ctl --help-subdev > "$E/v4l2-subdev-help.txt"
media-ctl --help > "$E/media-ctl-help.txt" 2>&1

if command -v RkLunch-stop-RKIPC_RV1126B.sh >/dev/null 2>&1; then
    RkLunch-stop-RKIPC_RV1126B.sh
fi

fuser -v /dev/video* /dev/v4l-subdev* 2>&1 |
    tee "$E/device-owners.txt"
```

Identify and stop any remaining owner deliberately; do not use an indiscriminate `killall`.

**VERIFIED:** the vendor documents the RKIPC stop script. Its example `/dev/video13` is an ISP mainpath example, not a stable identifier for your RAW node. [Aura CSI guide, lines 174–193](https://wiki.luckfox.com/Luckfox-Aura/Debian13/CSI/). ([Luckfox][18])

### 10.2 Establish probe and graph separately

```bash
dmesg > "$E/dmesg-before.txt"
i2cdetect -l | tee "$E/i2c-adapters.txt"

for d in /sys/bus/i2c/devices/*-001a; do
    [ -e "$d" ] || continue
    echo "=== $d ==="
    cat "$d/name" 2>/dev/null || true
    readlink -f "$d/driver" || true
    readlink -f "$d/of_node" || true
done | tee "$E/sensor-bindings.txt"

v4l2-ctl --list-devices | tee "$E/video-devices.txt"

for m in /dev/media*; do
    [ -e "$m" ] || continue
    media-ctl -d "$m" -p > "$E/$(basename "$m")-topology.txt"
done
```

Select the controller containing the actual H3 sensor and RKCIF path. Use the entity names printed by that graph:

```bash
read -r -p "Media device, e.g. /dev/mediaN: " MEDIA
read -r -p "Exact IMX296 sensor entity name: " SENSOR
read -r -p "Exact RKCIF RAW capture entity name: " RAW_ENTITY

SD="$(media-ctl -d "$MEDIA" -e "$SENSOR")"
VID="$(media-ctl -d "$MEDIA" -e "$RAW_ENTITY")"

test -c "$SD"
test -c "$VID"

printf 'MEDIA=%s\nSENSOR=%s\nSD=%s\nRAW_ENTITY=%s\nVID=%s\n' \
    "$MEDIA" "$SENSOR" "$SD" "$RAW_ENTITY" "$VID" |
    tee "$E/selected-nodes.txt"

media-ctl -d "$MEDIA" -p | tee "$E/selected-topology-before.txt"
```

If a required, non-immutable link is disabled, enable **the exact printed link** using:

```bash
# Populate these from the actual graph, not an example topology.
read -r -p "Exact source entity: " SRC_ENTITY
read -r -p "Source pad number: " SRC_PAD
read -r -p "Exact sink entity: " SINK_ENTITY
read -r -p "Sink pad number: " SINK_PAD

media-ctl -d "$MEDIA" --links \
    "\"$SRC_ENTITY\":$SRC_PAD -> \"$SINK_ENTITY\":$SINK_PAD [1]"
```

Do not create parallel competing links or overwrite immutable ones.

### 10.3 Enumerate the sensor—not just the video node

```bash
v4l2-ctl -d "$SD" --list-subdev-mbus-codes=pad=0 |
    tee "$E/sensor-mbus-codes.txt"

v4l2-ctl -d "$SD" --list-ctrls-menus |
    tee "$E/sensor-controls.txt"

v4l2-ctl -d "$VID" --list-formats-ext |
    tee "$E/capture-formats.txt"
```

After confirming `SBGGR10_1X10` in the output, use its reported numeric code. For the expected Linux code:

```bash
CODE=0x3007   # Use only after enumeration confirms this code.

v4l2-ctl -d "$SD" \
    --list-subdev-framesizes=pad=0,code="$CODE" |
    tee "$E/sensor-framesizes.txt"
```

The syntax is supported by `gjasny/v4l-utils`, `utils/v4l2-ctl/v4l2-ctl-subdev.cpp`, lines 42–94. Preserve the installed tool’s help output because its version is part of the evidence. ([GitHub][19])

Absence of frame-interval enumeration is not, by itself, failure of the proposed upstream-derived sensor driver. The required Rockchip **getter**, however, must function if the actual host calls it.

### 10.4 Select the native sensor mode

Select the full-array crop, then the full native format:

```bash
v4l2-ctl -d "$SD" \
  --set-subdev-selection=pad=0,target=crop,left=0,top=0,width=1456,height=1088

media-ctl -d "$MEDIA" --set-v4l2 \
  "\"$SENSOR\":0 [fmt:SBGGR10_1X10/1456x1088 field:none]"

v4l2-ctl -d "$SD" --get-subdev-fmt=pad=0 |
    tee "$E/native-sensor-format.txt"

v4l2-ctl -d "$SD" --get-subdev-selection=pad=0,target=crop |
    tee "$E/native-sensor-crop.txt"

media-ctl -d "$MEDIA" -p |
    tee "$E/native-topology.txt"
```

Verify all relevant pads report a compatible format. Some bridge subdevices forward or expose read-only formats; do not insist every bridge implements an independent writable `S_FMT`.

### 10.5 Establish RAW packing before capturing image evidence

**VERIFIED reference:** Rockchip exposes `RKCIF_CMD_GET_CSI_MEMORY_MODE` and `RKCIF_CMD_SET_CSI_MEMORY_MODE`, with compact, low-aligned word, and high-aligned word modes. Source: `rockchip-linux/kernel`, `develop-6.1`, [`include/uapi/linux/rkcif-config.h`, lines 21–25 and 84–102](https://github.com/rockchip-linux/kernel/blob/develop-6.1/include/uapi/linux/rkcif-config.h#L21-L25). **UNRESOLVED:** the exact shipping header/implementation until extracted. ([GitHub][20])

Export UAPI headers from the **actual selected kernel**, using its verified architecture/build environment:

```bash
make -C "$K" ARCH=arm64 headers_install \
    INSTALL_HDR_PATH=/work/aura-evidence/exported-uapi
```

Copy those exported headers and the helper source to the board. Compile against them:

```bash
cc -O2 -Wall -Wextra -Werror \
    -I/path/to/exported-uapi/include \
    cif_memory_mode.c -o cif-memory-mode
```

With all capture clients stopped:

```bash
./cif-memory-mode "$VID" | tee "$E/native-memory-mode-before.txt"
./cif-memory-mode "$VID" --set-low-align |
    tee "$E/native-memory-mode-set.txt"

# Re-issue S_FMT after changing memory mode.
v4l2-ctl -d "$VID" \
    --set-fmt-video=width=1456,height=1088,pixelformat=BG10

v4l2-ctl -d "$VID" --get-fmt-video |
    tee "$E/native-capture-format.txt"

./cif-memory-mode "$VID" |
    tee "$E/native-memory-mode-readback.txt"
```

If the header, ioctl, or requested mode is absent, **stop and inspect the actual BSP**. Do not substitute private ioctl numbers from another kernel.

**Calculated expectations, not substitutes for readback:**

| Mode        | Low-aligned 16-bit words, without extra padding |             Reference compact-mode stride calculation |
| ----------- | ----------------------------------------------: | ----------------------------------------------------: |
| 1456 × 1088 |          2912 bytes/line; 3,168,256 bytes/frame | 2048 bytes/line with the inspected 256-byte alignment |
| 728 × 544   |            1456 bytes/line; 792,064 bytes/frame |                   1024 bytes/line with that alignment |

Use actual `bytesperline`, `sizeimage`, plane count, data offset, and dequeued `bytesused`. The compact and word calculations come from different storage modes; do not mix them.

### 10.6 Prove correct native RAW

Set modest controls only after checking their enumerated ranges:

```bash
v4l2-ctl -d "$SD" \
    --set-ctrl=exposure=100,analogue_gain=0,test_pattern=0

v4l2-ctl -d "$SD" --get-ctrl=exposure,analogue_gain,vertical_blanking |
    tee "$E/native-controls.txt"
```

Capture a small native sample:

```bash
set -o pipefail

timeout 60s v4l2-ctl -d "$VID" \
    --stream-mmap=4 --stream-poll \
    --stream-count=12 --stream-to="$E/native.raw" --verbose \
    2>&1 | tee "$E/native-capture.log"
```

Record the actual layout:

```bash
read -r -p "Native bytesperline from G_FMT: " NATIVE_BPL
read -r -p "Native one-plane bytesused/frame record: " NATIVE_FRAME_BYTES

python3 check_frames.py "$E/native-capture.log" \
    --expected-frames 12 --expected-bytes "$NATIVE_FRAME_BYTES" |
    tee "$E/native-frame-audit.json"

python3 inspect_word_raw.py "$E/native.raw" \
    --width 1456 --height 1088 \
    --stride "$NATIVE_BPL" --frame-bytes "$NATIVE_FRAME_BYTES" \
    --frame-index 3 --output "$E/native-mosaic.png" |
    tee "$E/native-raw-statistics.json"
```

The preview helper does **not** demosaic or run an ISP. Confirm a recognizable scene, correct row layout, plausible 10-bit values, response to covering the lens, and agreement between declared Bayer phase and controlled colored targets.

Next, exercise the enumerated test-pattern menu and compare its expected geometry/data against the decoded RAW. Return `test_pattern` to zero and prove the live scene returns.

**Important:** `v4l2-ctl` can discard error buffers while continuing toward its successful-buffer count. A zero process exit or `stream-count` completion is not enough. Inspect all verbose dequeues and kernel messages. Primary implementation: `gjasny/v4l-utils`, [`v4l2-ctl-streaming.cpp`, lines 565–622 and the capture error-buffer handling](https://github.com/gjasny/v4l-utils/blob/master/utils/v4l2-ctl/v4l2-ctl-streaming.cpp#L565-L622). ([GitHub][21])

### 10.7 Prove controls

**STRONG INFERENCE — test procedure:** under fixed illumination, capture separate runs at two valid exposure settings while holding gain constant. Then compare two valid gain settings at fixed exposure. Save control readback and RAW for each run.

Require directional image response without interpreting saturated or nearly black frames as useful evidence. Test-pattern enable/disable is an additional independent control test.

Do not treat changed values in `--get-ctrl` as proof that the sensor registers and image changed.

### 10.8 Establish 10 Hz with VBLANK

For the inspected baseline:

$$
f = \frac{74{,}250{,}000}
         {1100\,(\text{height}+\text{VBLANK})}
$$

Thus nominal 10 Hz requires 6750 total lines:

```text
1456 × 1088: VBLANK = 6750 − 1088 = 5662
 728 ×  544: VBLANK = 6750 −  544 = 6206
```

These are **STRONG INFERENCE calculations from the inspected register programming**, not measured Aura timing. Recompute if the port changes HMAX or timing configuration. **VERIFIED source:** upstream IMX296 HMAX/control/setup code, particularly lines 383–408 and 513–532. ([GitHub][1])

First test the native mode at nominal 10 Hz:

```bash
v4l2-ctl -d "$SD" --set-ctrl=vertical_blanking=5662
v4l2-ctl -d "$SD" --set-ctrl=exposure=200,analogue_gain=0

v4l2-ctl -d "$SD" \
    --get-ctrl=vertical_blanking,horizontal_blanking,pixel_rate,exposure,analogue_gain |
    tee "$E/native-10hz-controls.txt"

v4l2-ctl -d "$SD" --get-subdev-fps=pad=0 |
    tee "$E/native-10hz-reported-fps.txt"

timeout 90s v4l2-ctl -d "$VID" \
    --stream-mmap=4 --stream-poll \
    --stream-count=600 --stream-to=/dev/null --verbose \
    2>&1 | tee "$E/native-10hz.log"
```

The getter should report the actual programmed interval after the Rockchip adaptation. Do not implement it as a hard-coded “10 fps” report.

For rate grading, record an approved tolerance derived from the module clock specification or measurement and the timestamp-measurement budget:

```bash
read -r -p "Approved FPS tolerance in ppm, from recorded timing budget: " FPS_TOLERANCE_PPM

python3 check_frames.py "$E/native-10hz.log" \
    --expected-frames 600 --expected-bytes "$NATIVE_FRAME_BYTES" \
    --target-fps 10 --fps-tolerance-ppm "$FPS_TOLERANCE_PPM" |
    tee "$E/native-10hz-audit.json"
```

Without a tolerance, the helper reports the measured rate but deliberately does not issue a rate pass.

V4L2 timestamps are not automatically equivalent to the Pi’s `SensorTimestamp` or a physical exposure-start measurement. Preserve their flags and establish the shipping driver’s timestamp source.

### 10.9 Select and prove 728 × 544 / 10 Hz

Stop streaming, reassert full-array crop, and change the **sensor** format:

```bash
v4l2-ctl -d "$SD" \
  --set-subdev-selection=pad=0,target=crop,left=0,top=0,width=1456,height=1088

media-ctl -d "$MEDIA" --set-v4l2 \
  "\"$SENSOR\":0 [fmt:SBGGR10_1X10/728x544 field:none]"

./cif-memory-mode "$VID" --set-low-align

v4l2-ctl -d "$VID" \
    --set-fmt-video=width=728,height=544,pixelformat=BG10

v4l2-ctl -d "$SD" --set-ctrl=vertical_blanking=6206
v4l2-ctl -d "$SD" --set-ctrl=exposure=200,analogue_gain=0,test_pattern=0

v4l2-ctl -d "$SD" --get-subdev-fmt=pad=0 |
    tee "$E/binned-sensor-format.txt"

v4l2-ctl -d "$SD" --get-subdev-selection=pad=0,target=crop |
    tee "$E/binned-sensor-crop.txt"

v4l2-ctl -d "$SD" --get-subdev-fps=pad=0 |
    tee "$E/binned-reported-fps.txt"

v4l2-ctl -d "$VID" --get-fmt-video |
    tee "$E/binned-capture-format.txt"

./cif-memory-mode "$VID" |
    tee "$E/binned-memory-mode.txt"

media-ctl -d "$MEDIA" -p |
    tee "$E/binned-topology.txt"
```

Repeat the small RAW sample and control tests at this mode, using its actual stride and frame size. Then:

```bash
read -r -p "Binned one-plane bytesused/frame record: " BINNED_FRAME_BYTES

timeout 90s v4l2-ctl -d "$VID" \
    --stream-mmap=4 --stream-poll \
    --stream-count=600 --stream-to=/dev/null --verbose \
    2>&1 | tee "$E/binned-10hz.log"

python3 check_frames.py "$E/binned-10hz.log" \
    --expected-frames 600 --expected-bytes "$BINNED_FRAME_BYTES" \
    --target-fps 10 --fps-tolerance-ppm "$FPS_TOLERANCE_PPM" |
    tee "$E/binned-10hz-audit.json"
```

### 10.10 Restart and soak budget

**STRONG INFERENCE — proposed Nuvo acceptance budget, not a vendor specification:** adopt 50 stream stop/start cycles, 10 cold boots, and an 18,000-frame binned-mode run. Record agreement to that budget before calling it a project gate.

```bash
for i in $(seq -w 1 50); do
    timeout 15s v4l2-ctl -d "$VID" \
        --stream-mmap=4 --stream-poll \
        --stream-count=50 --stream-to=/dev/null --verbose \
        > "$E/restart-$i.log" 2>&1 || exit 1

    python3 check_frames.py "$E/restart-$i.log" \
        --expected-frames 50 --expected-bytes "$BINNED_FRAME_BYTES" \
        > "$E/restart-$i.json" || exit 1
done

timeout 1900s v4l2-ctl -d "$VID" \
    --stream-mmap=4 --stream-poll \
    --stream-count=18000 --stream-to=/dev/null --verbose \
    2>&1 | tee "$E/binned-soak.log"

python3 check_frames.py "$E/binned-soak.log" \
    --expected-frames 18000 --expected-bytes "$BINNED_FRAME_BYTES" \
    --target-fps 10 --fps-tolerance-ppm "$FPS_TOLERANCE_PPM" |
    tee "$E/binned-soak-audit.json"

dmesg > "$E/dmesg-after.txt"
journalctl -b --no-pager > "$E/journal-after.txt"
```

Collect separate evidence after each cold boot; do not merely count power cycles.

---

## 11. Pass/fail table for normal single-camera operation

All entries below are **proposed engineering gates**. No hardware gate has passed during this review.

| Milestone                                   | Pass evidence                                                                                                  | Fail or unresolved condition                                                                   |
| ------------------------------------------- | -------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| Baseline/recovery prerequisite              | Identified vendor image boots; unmodified SDK image boots; Loader and recovery package demonstrated            | Unknown image/SDK identity, wrong medium, or untested rollback                                 |
| **1. Sensor probes**                        | Correct driver bound to the H3 device; expected power/clock behavior; valid sensor communication               | Binding forced through DT alone, unexplained model-read failure, deferred probe, or I²C errors |
| **2. Media graph complete**                 | Reciprocal endpoints, correct RX0 path, one lane, proper RAW node, compatible active formats                   | Missing/wrong link, wrong logical PHY, orphaned sensor, competing old camera                   |
| **3. Native RAW correct**                   | Enumerated native mode; verified memory layout; recognizable scene and test-pattern evidence                   | Merely receiving bytes, incorrect packing/stride, blank or corrupted image                     |
| **4. Controls work**                        | Exposure/gain/test-pattern changes affect RAW as expected and read back correctly                              | Cached control changes with no image effect, stale exposure after VBLANK change                |
| **5. Repeated free-running capture stable** | Agreed restart/boot tests pass; no unexplained sequence gaps, error buffers, or accumulating host errors       | Timeouts, runtime-PM restart failure, CSI errors, unexpected resets                            |
| **6. 728 × 544 / 10 Hz works**              | Sensor—not just video node—uses binned mode; correct RAW; measured cadence within approved budget; soak passes | Host-only crop, wrong timing, ungraded tolerance, malformed frames                             |
| **7. ISP/color preview, optional**          | Correct ISP graph and appropriate tuning produce acceptable color                                              | Missing IQ/color support; **does not invalidate milestones 1–6**                               |

A RAW pass is the camera-operation authority. A processed preview alone cannot substitute for it.

---

## 12. Failure-localization table

The interpretations below are **STRONG INFERENCE diagnostic directions**, not conclusions from a single error code.

| Symptom                                                       | First place to investigate                                                             | Do not assume                                           |
| ------------------------------------------------------------- | -------------------------------------------------------------------------------------- | ------------------------------------------------------- |
| No Loader device                                              | Data versus power port, USB cable, driver installation, button sequence, supply        | Camera software caused it                               |
| Wrong root device or image appears unchanged                  | Boot medium, bootloader selection, packaged image identity, running DTB                | Flash success proves the new kernel booted              |
| Sensor not instantiated                                       | Running DTB, actual I²C controller, node status, compatible, driver config             | A module-loading command can repair a missing DT node   |
| I²C `-ENXIO` / `-EREMOTEIO`                                   | Cable, 3.3 V, enable polarity, reset, oscillator, address, pull-up compatibility       | Every such failure is an address problem                |
| Persistent deferred probe                                     | Missing clock/regulator/GPIO provider and dependency chain                             | The sensor driver needs arbitrary delays                |
| Generic model detection fails                                 | Actual model-register readability, power/clock/reset, I²C behavior                     | Force `imx296lq` until the error disappears             |
| “No pixel rate control in subdev” in the inspected D-PHY path | Missing **LINK_FREQ menu**, despite the misleading text                                | Adding another PIXEL_RATE control fixes it              |
| `get_mbus_config` failure                                     | Sensor callback/API and endpoint parsing                                               | The DTS alone satisfies the host                        |
| `g_frame_interval` failure                                    | Missing getter or invalid current timing state                                         | `--set-parm=10` is the required fix                     |
| STREAMON fails with complete-looking graph                    | Host callbacks, lane mode, format validation, clock/rate configuration                 | A printed graph proves streaming compatibility          |
| STREAMON succeeds but no frames arrive                        | Sensor streaming state, physical D0/clock, enable/reset, D-PHY lane selection          | Missing ISP IQ necessarily caused it                    |
| CSI CRC/ECC or frame-boundary errors                          | Lane integrity, cable orientation, clock/settle configuration, power quality           | Software timeouts should simply be increased            |
| Image has repeated stripes or wrong row lengths               | Compact versus word mode, high versus low alignment, actual stride                     | Bayer order is the only issue                           |
| Image looks plausible but colors are wrong                    | Bayer phase, crop offsets, flips, decoder settings                                     | ISP tuning can compensate for a wrongly declared mosaic |
| Cadence wrong but image correct                               | VMAX/HMAX, VBLANK readback, frame getter, clock accuracy, host frame dropping          | Capture-node FPS settings changed sensor timing         |
| First run works; restart fails                                | Runtime-PM sequencing, module-enable ownership, reset/oscillator startup, stream state | A full reboot is an acceptable permanent fix            |
| RAW passes; RKAIQ complains about tuning                      | Optional ISP/tuning phase                                                              | Sensor bring-up failed                                  |

For the three host-interface failures, inspect the exact shipping equivalents of the D-PHY and RKCIF call sites cited in §5 before changing code.

---

## 13. Evidence bundle to return to the Nuvo developer

Return a structured, private evidence package; do not redistribute a licensed SDK publicly without permission.

```text
aura-single-imx296-evidence/
├── 00-downloads/
│   ├── artifact-record.txt
│   ├── official-release-notes/
│   ├── published-checksums/
│   └── locally-calculated-hashes.txt
├── 01-hardware/
│   ├── aura-sku-and-pcb-photos/
│   ├── actual-module-sku-and-pcb-photos/
│   ├── vendor-electrical-confirmation/
│   ├── cable-continuity.csv
│   └── power-enable-clock-measurements/
├── 02-sdk-identity/
│   ├── manifest-pinned.xml
│   ├── projects.txt
│   ├── kernel-identity.txt
│   ├── actual-aura-dts-and-includes/
│   ├── stock-board-config.mk
│   └── host-and-toolchain-versions/
├── 03-vendor-baseline/
│   ├── flash-log/
│   ├── uart-boot.log
│   ├── running.dtb
│   ├── running.dts
│   └── storage-and-os-identity/
├── 04-unmodified-sdk-build/
│   ├── lunch-and-build-logs/
│   ├── image-hashes/
│   └── comparison-with-vendor-image.md
├── 05-nuvo-source/
│   ├── driver-provenance.txt
│   ├── patches/
│   ├── nuvo-board-config.mk
│   ├── final.config
│   ├── final-dts-and-dtsi/
│   └── dtb-build-dependencies/
├── 06-custom-image/
│   ├── package-manifest/
│   ├── component-hashes/
│   ├── update.img.sha256
│   └── recovery-test-log/
├── 07-native-raw/
├── 08-controls/
├── 09-binned-10hz/
├── 10-restarts-coldboots-soak/
├── acceptance.md
└── SHA256SUMS
```

Include the full native and binned media topologies; source/capture formats; memory-mode readback; stride/sizeimage/bytesused; RAW samples; control readbacks; verbose dequeue logs; timing calculations and tolerance justification; and unfiltered kernel logs.

The source record must distinguish:

```text
Upstream baseline tag/commit
Selected Aura SDK/kernel identity
Vendor modifications already present
Nuvo changes
Uncommitted changes, if any
Exact compiler and packaging tool versions
```

Hash the completed evidence package:

```bash
cd /path/to/aura-single-imx296-evidence

find . -type f ! -name SHA256SUMS -print0 |
    sort -z |
    xargs -0 sha256sum > SHA256SUMS
```

Freeze three separate identities: **downloaded vendor image**, **unmodified SDK build**, and **passing Nuvo single-camera image**. Do not overwrite one with another under a generic `update.img` archive name.

---

## 14. Hard exit gate before dual IMX296 work

**The next phase is authorized only when milestones 1–6 pass on an identified, reproducible, recoverable image and the evidence package is complete.**

Specifically, the frozen image must establish one correctly identified module on H3, the verified RX0 graph, correct native RAW, effective controls, repeatable free-running operation, and sensor-binned 728 × 544 capture at measured nominal 10 Hz within an approved timing budget.

At that freeze point:

```text
CSI1 / H4 camera path: disabled and unconnected
Second IMX296 node: absent or disabled
External-trigger driver patch: not applied
Trigger-mode activation: absent
XTR generator and wiring: not used
Stereo synchronization claims: none
Calibration: not started
Nuvo daemon / transport integration: not started
ISP/color tuning: optional separate result, not the RAW pass authority
```

**Current exit-gate status: UNRESOLVED / NOT PASSED.** The missing cloud artifacts, actual Aura BSP, and exact module electrical evidence prevent final patch approval. Public reference sources establish a credible path and expose specific integration hazards, but they do not replace those inputs.

### **What the user should do next at the bench**

1. **Photograph the Aura SKU/PCB revision, both sides of the InnoMaker module, and the actual cable. Do not connect the camera yet.**
2. **Download the current official Debian image and SDK through the official folder; record filenames, URLs, sizes, release notes, and hashes.**
3. **Install the verified Windows tools, establish Loader/recovery, and boot the appropriate vendor image with the camera disconnected.**
4. **Extract the SDK and return the pinned manifest, kernel identity, actual Aura DTS/include closure, and selected Debian board configuration.**
5. **Resolve module enable/clock/reset behavior and verify H3-to-module continuity using signals—not the previous pin-number table.**
6. **Only then approve the small driver/DTS patch, execute the native-to-binned RAW tests, and freeze the passing image before any dual-camera work.**

[1]: https://raw.githubusercontent.com/torvalds/linux/v6.3/drivers/media/i2c/imx296.c "https://raw.githubusercontent.com/torvalds/linux/v6.3/drivers/media/i2c/imx296.c"
[2]: https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/drivers/phy/rockchip/phy-rockchip-csi2-dphy.c "https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/drivers/phy/rockchip/phy-rockchip-csi2-dphy.c"
[3]: https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/drivers/media/platform/rockchip/cif/capture.c "https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/drivers/media/platform/rockchip/cif/capture.c"
[4]: https://wiki.luckfox.com/Luckfox-Aura/Downloads/ "https://wiki.luckfox.com/Luckfox-Aura/Downloads/"
[5]: https://github.com/LuckfoxTECH/luckfox-aura-docs/releases/expanded_assets/v0.0.1 "https://github.com/LuckfoxTECH/luckfox-aura-docs/releases/expanded_assets/v0.0.1"
[6]: https://wiki.luckfox.com/Luckfox-Aura/Introduction/ "https://wiki.luckfox.com/Luckfox-Aura/Introduction/"
[7]: https://www.inno-maker.com/product/cam-imx296color/ "https://www.inno-maker.com/product/cam-imx296color/"
[8]: https://wiki.luckfox.com/Luckfox-Aura/SDK-Image-Compilation/ "https://wiki.luckfox.com/Luckfox-Aura/SDK-Image-Compilation/"
[9]: https://raw.githubusercontent.com/torvalds/linux/master/drivers/media/i2c/imx296.c "https://raw.githubusercontent.com/torvalds/linux/master/drivers/media/i2c/imx296.c"
[10]: https://raw.githubusercontent.com/torvalds/linux/v6.3/Documentation/devicetree/bindings/media/i2c/sony%2Cimx296.yaml "https://raw.githubusercontent.com/torvalds/linux/v6.3/Documentation/devicetree/bindings/media/i2c/sony%2Cimx296.yaml"
[11]: https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.1.y/arch/arm/boot/dts/overlays/imx296-overlay.dts "https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.1.y/arch/arm/boot/dts/overlays/imx296-overlay.dts"
[12]: https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/drivers/media/i2c/imx415.c "https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/drivers/media/i2c/imx415.c"
[13]: https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/arch/arm64/boot/dts/rockchip/rv1126b-pinctrl.dtsi "https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/arch/arm64/boot/dts/rockchip/rv1126b-pinctrl.dtsi"
[14]: https://docs.inno-maker.com/home/mipi-cameras/imx296-mipi-cameras "https://docs.inno-maker.com/home/mipi-cameras/imx296-mipi-cameras"
[15]: https://forums.luckfox.com/viewtopic.php?t=2377 "https://forums.luckfox.com/viewtopic.php?t=2377"
[16]: https://wiki.luckfox.com/Luckfox-Aura/Getting-Started/ "https://wiki.luckfox.com/Luckfox-Aura/Getting-Started/"
[17]: https://wiki.luckfox.com/assets/images/maskrom-18611ff5ad8ed2ec88715d94dfe8d1ae.webp "https://wiki.luckfox.com/assets/images/maskrom-18611ff5ad8ed2ec88715d94dfe8d1ae.webp"
[18]: https://wiki.luckfox.com/Luckfox-Aura/Debian13/CSI/ "https://wiki.luckfox.com/Luckfox-Aura/Debian13/CSI/"
[19]: https://raw.githubusercontent.com/gjasny/v4l-utils/master/utils/v4l2-ctl/v4l2-ctl-subdev.cpp "https://raw.githubusercontent.com/gjasny/v4l-utils/master/utils/v4l2-ctl/v4l2-ctl-subdev.cpp"
[20]: https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/include/uapi/linux/rkcif-config.h "https://raw.githubusercontent.com/rockchip-linux/kernel/develop-6.1/include/uapi/linux/rkcif-config.h"
[21]: https://raw.githubusercontent.com/gjasny/v4l-utils/master/utils/v4l2-ctl/v4l2-ctl-streaming.cpp "https://raw.githubusercontent.com/gjasny/v4l-utils/master/utils/v4l2-ctl/v4l2-ctl-streaming.cpp"
