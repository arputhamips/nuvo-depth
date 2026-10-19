# First signed install release qualification â€” 2026-10-19

This record covers the GitHub package build, signed release contract and a
bounded terminal upgrade on the existing Pi 5. It is not a factory acceptance
certificate or a claim that every onboarding/network/recovery scenario was
exercised on physical hardware. The first public release is a beta.

## Platform provenance

| Component | Observed before qualification |
| --- | --- |
| Board | Raspberry Pi 5 Model B Rev 1.1, 16 GiB |
| OS | Raspberry Pi OS Lite 64-bit, Debian 13/Trixie, updated userspace |
| Running kernel | `6.12.75+rpt-rpi-2712` |
| Image origin | Raspberry Pi reference 2026-04-21, pi-gen `4ad56cc850fa60adcc7f07dc15879bc95cc1d281`, stage2 |
| Camera library | `0.7.2+rpt20260817-1` |
| OpenCV | `4.10.0+dfsg-5` |
| Cameras | Two IMX296 RGGB, 1456 x 1088 native mode |
| Installed Nuvo cohort | `0.1.0`, with local engineering updates |
| Throttle state | `0x0` |

The image identity and pi-gen commit in `/etc/rpi-issue` and
`/boot/firmware/issue.txt` match the upstream image's `.info` file. The upstream
image includes kernel 6.12.75. The official image is:

[2026-04-21 Raspberry Pi OS Lite ARM64 image](https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-04-21/2026-04-21-raspios-trixie-arm64-lite.img.xz)

The upstream [SHA-256 sidecar](https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-04-21/2026-04-21-raspios-trixie-arm64-lite.img.xz.sha256)
reports `4cd31df026fd82243805a326dc0cafd7383f7e3d30c9413e7044d507aae281e2`.
This establishes the installed image's origin; the running SD card has since
received package and application updates. It is not a byte-for-byte clean image
or a qualification of the current upstream latest image/kernel.

## Acceptance results

| Check | Result |
| --- | --- |
| GitHub release key | Random challenge signed and verified against the pinned public key in the main-only environment |
| Native ARM64 package build | 55/55 CTest entries passed; all eight runtime packages built |
| Clean package install | Passed in the disposable Trixie container; dependency resolution, post-install hooks, installed config/helper startup and dpkg audit passed |
| Signed terminal dry run | Manifest signature, package hashes, package control metadata and Pi preflight passed without applying a plan |
| Terminal upgrade | Completed through the detached systemd worker; current network retained, ownership deferred, accepted calibration retained |
| Factory separation | `--factory --dry-run` rejected the pre-existing legacy installation before package/network changes |
| Reboot | Returned on the same kernel with NetworkManager, gateway and native capture active and no failed units |
| Capture restart | 3 cycles x 100 stereo pairs, epochs 2/4/6, zero outstanding leases or pairer/callback/trigger/requeue errors |
| Sensor timestamp skew | Maximum consumed delta 21,000 ns (21 us) over the three bounded cycles |
| HTTPS and setup IPC | Certificate-verified console, health, capabilities, setup status and browser login passed |
| Raw SDK decode | 396,881-byte NuvoStream response decoded as `left_mono`, 728 x 544, calibration authority 1 |
| Preservation | All 17 existing configuration/identity/calibration hashes unchanged |
| Throttling | `0x0` after upgrade, reboot and capture tests |

Initial runtime acceptance used the hosted build at source `1e1a6fe`:
[package build and clean install](https://github.com/arputhamips/nuvo-depth/actions/runs/39315825099).
The [signing check](https://github.com/arputhamips/nuvo-depth/actions/runs/39316157236)
exercised the configured production release key without publishing assets.
The final tagged release is rebuilt by the signed-release workflow and its
signed draft is checked before promotion to a public beta.

The initial unpublished beta 1 draft exposed GitHub normalizing `~` in uploaded
asset filenames. Download-and-verify caught the mismatch. The release builder
now normalizes names before signing, with a real Debian beta-package regression
test. Beta 2 is the first public install release; the beta 1 source tag remains
as immutable engineering history.

The updated libcamera package changed its public C++ Span ABI while retaining
upstream version 0.7.2 and SONAME 0.7. A load test rejected the October-built
Nuvo binary against the Pi's August library before installation. The tested
cohort is now pinned by the Pi platform package and ARM64 build dependency:

- `libcamera0.7` and `libcamera-ipa`: `0.7.2+rpt20261009-1`;
- `libcamera-dev` on the builder: `0.7.2+rpt20261009-1`;
- `rpicam-apps-core` and its matching `librpicam-app1`: `1.14.0-1`.

The Pi's camera packages, including Python bindings and the installed Lite
metapackage, were upgraded together; older `.deb` files were retained privately
under the same backup directory. This was a userspace upgrade, with no full OS
or kernel upgrade. Fresh installs obtain the required camera cohort through APT.

The saved output mode was encoded-only. Raw mode was enabled temporarily for
the decoder test and then restored byte-for-byte. Existing administrative-token
login remained compatible, and the pending setup code/owner state was not
consumed or changed by the browser check. The first cold boot needed one
service startup retry; it recovered automatically. Subsequent bounded capture
restarts passed. The installer now reports a completed required reboot correctly
and explicitly reinstalls its selected Nuvo cohort on a retry.

## Final public beta 2 verification

[Signed release workflow](https://github.com/arputhamips/nuvo-depth/actions/runs/39318514456)
built, tested, installed and signed tag `v0.1.1-beta.2`, commit
`e436832d89b38baf9d5549a585c39f644d1e99fe`. The downloaded draft passed Ed25519
verification, SHA-256/size checks for all 18 signed assets, exact metadata checks
for all eight runtime packages, and comparison of all 399 source files with
the tagged Git tree.

That exact signed draft installed on the Pi as `0.1.1~beta.2` and passed the
HTTPS/browser/setup-IPC checks, a 396,881-byte 728 x 544 raw frame decode, and
another three capture cycles of 100 stereo pairs each. Maximum consumed sensor
skew was 19 us; pairer/callback/trigger/requeue error counts and outstanding
leases were zero. All 17 preserved-file hashes matched and throttling stayed
`0x0`.

The release was then [published as a prerelease](https://github.com/arputhamips/nuvo-depth/releases/tag/v0.1.1-beta.2).
Anonymous HTTPS downloads matched the signed draft byte-for-byte. The standalone
shell installer then ran directly from those GitHub URLs on the Pi, verified
its Python archive and all packages, and successfully reinstalled the same
version through the detached worker. The dpkg log confirmed all eight
same-version reinstalls. The final receipt was complete, network access was
confirmed, no reboot was pending, and the gateway/capture/NetworkManager services
were active. Identity, certificate, existing administrative access, calibration
and the original encoded-only output setting were preserved.

## Preservation and remaining gates

Before any package change, the Pi retained a root-only backup at
`/var/backups/nuvo-depth-release-20261019/before.tar.gz` (45,715,557 bytes), plus
before-state hashes for configuration, identity and selected calibration files.
The backup contains installed Nuvo files, unit overrides, boot policy and device
state; it is deliberately not a public release artifact. Network credentials,
SSH credentials, owner secrets and factory keys are not part of public evidence.

The beta does not establish fresh-SD hotspot onboarding, Wi-Fi handoff rollback,
power-cut recovery, USB power/data compatibility, electrical/trigger-scope
approval, sustained thermal endurance or metric calibration for other units.
Those remain separate hardware/manufacturing gates. The installer still requires
accepted per-unit calibration or explicit provisional engineering preview before
starting the supported capture head. Custom/incomplete heads can install the console.
