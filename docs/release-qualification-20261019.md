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

The initial install/capture checks did not exercise hotspot onboarding; the
setup acceptance below adds that coverage. Freshly flashed SD acceptance,
physical power-cut recovery, USB power/data compatibility, electrical/trigger-scope
approval, sustained thermal endurance or metric calibration for other units.
Those remain separate hardware/manufacturing gates. The installer still requires
accepted per-unit calibration or explicit provisional engineering preview before
starting the supported capture head. Custom/incomplete heads can install the console.

## UF-01 and UF-02 physical setup acceptance

Later on 19 October, the same Pi was used for explicit first-use tests. A separate
root-only backup at `/var/backups/nuvo-depth-uf12-20261019/before.tar.gz`
(49,318,373 bytes) preserved the working unit before temporarily replacing Nuvo's
identity, ownership and provisioning state. These are fresh **Nuvo state** tests
on the existing qualified OS, not a newly flashed SD image. No Ethernet cable was
connected to the Pi. The client used its Wi-Fi adapter for the setup AP and its
existing wired LAN for the destination-network confirmation.

The public beta 2 bootstrap installed `--factory --wifi-country US` without any
questions. The unit remained unclaimed, capture stayed gated with calibration
pending, and the protected AP was armed until reboot. After reboot the client
joined the unique WPA2/CCMP AP, obtained DHCP, and opened the actual setup page.
An intentionally nonexistent SSID caused the same AP/password to return after
the three-minute confirmation deadline. A valid retry joined the destination
network and was confirmed through the browser's `.local` link.

That test exposed a case-sensitive Host check: the OS hostname contained capitals
but browsers lowercase DNS names. The correction accepts case-insensitive DNS
names and optional port 80 while continuing to reject foreign origins/ports.
The recovered setup page now shows its rollback explanation and clears that
error on the next attempt. A second correction moves persistent Wi-Fi country
preparation into the installer, verifies the boot command line, and keeps the
sandboxed AP service limited to runtime regulatory/radio operations. Current
raspi-config writes `/boot/firmware/cmdline.txt`; it could silently fail that write
inside the earlier read-only service sandbox. Missing boot command lines are now
rejected during preflight. These fixes are tagged `v0.1.1-beta.3` at `46a58d1`.

The user manually verified the temporary device certificate, entered the short
setup code, chose a temporary owner password and reported that the console
opened. The real root ownership store confirmed a saved password verifier and
consumed pairing proof. Reusing that code through the HTTPS gateway returned
401. A normal terminal reinstall preserved that owner's store and device
identity byte-for-byte, retained the current network, and did not ask to set an
owner again. `--factory --dry-run` then rejected the owned unit before package
or network work.

A second fresh Nuvo-state fixture exercised UF-02's current-network selection.
The installer collected a masked owner password in the SSH terminal, completed
the signed public GitHub install, retained the existing connection, and recorded
terminal origin. Two certificate-verified password logins succeeded with
`needs_owner_password=false` and Secure/HttpOnly session cookies. The raw terminal
transcript was checked to ensure the owner password was never echoed.

The terminal-created owner and identity also survived a reboot on the confirmed
current network, with no AP reopening and no failed units. The signed beta 3
draft passed all 55 CTest entries, clean container installation and signing in
[release run 39322028865](https://github.com/arputhamips/nuvo-depth/actions/runs/39322028865).
The downloaded bundle passed signature and hash checks for 18 signed files,
all eight runtime package metadata checks, and comparison of all 399 source
files against commit `46a58d1dd3309d44ab5b8657bba36eafe3c91e4c`.

UF-02 then upgraded to those beta 3 packages while selecting the protected
hotspot option. It preserved the terminal origin, owner store and identity,
saved `cfg80211.ieee80211_regdom=US`, and kept SSH connected with the AP armed.
After reboot the new sandboxed service activated the AP successfully, served
the real browser setup page and reported no error or failed units. Ownership
remained claimed with the same password verifier.

During another browser-submitted handoff, a controlled reboot interrupted the
unconfirmed transaction. On the next boot, the same protected AP/password
returned, the stale confirmation capability was removed, and the owner store
was unchanged. The browser displayed the rollback explanation and allowed a
fresh attempt.

The valid retry confirmed through the actual beta 3 browser page. Password login
still worked without another owner questionnaire. A further reboot with the
original `setup_ap` policy retained its confirmed destination network, preserved
the owner/identity, and did not reopen the AP; no units failed.

A third fresh Nuvo-state fixture installed the final beta 3 factory preset
without selections, rebooted into its unique protected AP and remained
unclaimed. The real certificate-verified HTTPS gateway and installed root agent
accepted its short code, required a permanent password, saved that password
through the CSRF-protected owner endpoint, rejected the consumed code and
accepted a subsequent password login. This final ownership check used API
requests; the earlier certificate/code/password UI interaction was completed
manually by the user. Calibration correctly remained pending and shipping
readiness remained false on the temporary factory identity.

The [beta 3 prerelease](https://github.com/arputhamips/nuvo-depth/releases/tag/v0.1.1-beta.3)
was published after those checks. Anonymous downloads of all 20 release assets
matched the signed draft byte-for-byte. The original unit was then restored:
all 18 saved configuration/identity/calibration hashes and the saved boot command
line matched, its accepted calibration and existing network policy returned,
gateway/native capture/NetworkManager were active, no units failed, and
throttling was `0x0`. Temporary recovery timers and setup AP policy were removed.

The standalone beta 3 shell installer then completed an actual anonymous online
reinstall from the public GitHub URLs through its detached worker. All original
18 hashes still matched afterward. Certificate-verified console, health,
capabilities, setup IPC and existing administrative access passed. A fresh
396,880-byte NuvoStream response decoded as `left_mono`, 728 x 544, with accepted
calibration authority 1. The temporary raw-delivery setting was restored to the
original encoded-only configuration byte-for-byte.

This covers software provisioning and the exercised network/ownership paths.
It does not qualify a newly flashed card, physical power loss during dpkg/SD
writes, captive-portal discovery, enterprise Wi-Fi, USB power/data arrangements,
or manufacturing electrical/calibration/shipping acceptance. Device HTTPS still
requires the documented per-unit certificate verification by the user.
