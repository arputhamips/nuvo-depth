# User flow — Use case 2: install Nuvo on a user-owned Pi or SD card

> Browser-access update (v0.1.1-beta.5): the HTTPS console now opens directly,
> without a login, setup code or owner-password step. Ownership/login proposals
> below are historical and do not describe the current browser or installer.
> Network setup, certificate verification and calibration requirements still apply.

**Specification:** UF-02 · **Version:** 0.5 · **Updated:** 19 October 2026  
**Status:** Prompted terminal installer and factory preset published as a signed GitHub beta. Both network choices, terminal ownership and reboot recovery have [physical Pi setup evidence](../release-qualification-20261019.md#uf-01-and-uf-02-physical-setup-acceptance). See [commands and current limits](../install.md).

## User and outcome

The user is technical and can run commands on their Pi locally or over SSH with administrator access. Their Pi may already have Wi-Fi configured and unrelated software installed.

The flow is **boot the supported OS → run the install script → answer the required terminal questions → review and apply → reboot or switch access if selected → open the Nuvo web UI**. The script collects the information required for a complete supported setup and automates the installation. No Nuvo desktop application, graphical installer or separate bootstrap `.deb` is required.

Normal execution uses simple numbered terminal selections and masked secret input, with detected values prefilled. Ask for required information that cannot be detected; optional advanced controls stay optional. The network choice is **Use current network** (default) or **Set up network after installation using a Nuvo setup hotspot**. A `--factory` flag skips selections and applies the versioned manufacturing defaults through the same provisioning code. The web UI validates the result, displays live status, and handles intentionally deferred details or recovery without repeating completed questions.

Both paths follow the [shared setup contract](shared-setup-contract.md). Installation origin, selected network policy and ownership are independent. A normal terminal user may deliberately select a setup hotspot without creating a factory device. Reruns preserve existing valid selections and credentials unless the administrator explicitly reconfigures them. Unclaimed status, missing cameras and calibration recovery do not independently start a hotspot or reopen claiming.

Initial hardware scope is the supported **Pi 5 and dual IMX296 configuration**. Report three separate outcomes:

1. **Installed — ready for web setup:** compatible authentic packages, configuration services and a reachable setup endpoint are available.
2. **Cameras working:** the intended cameras and trigger produce fresh associated frames.
3. **Metric depth ready:** accepted calibration matches the device and actual camera assembly.

The first outcome can be complete while hardware-specific details or calibration are pending. Camera readiness must not be a prerequisite for opening the page needed to finish configuring the cameras.

## Supported OS and flashable base image

Provide a **Supported base OS image** download on the getting-started page. Recommend an exact qualified **Raspberry Pi OS Lite, 64-bit** image with **no Nuvo packages preinstalled**. The Trixie/arm64 family matches the repository's recorded baseline, but the exact image must be selected and qualified before publication as supported.

Use a Raspberry Pi disk image, normally `.img.xz` or extracted `.img`, rather than a PC ISO. Prefer a versioned official Raspberry Pi download plus Nuvo's tested-release manifest. There is no need to build a Nuvo-specific bare OS simply to redistribute a stock base. If availability later requires a mirror, preserve the qualified bytes and checksum. A preinstalled Nuvo appliance image is a separate optional distribution, not a requirement here.

| Publish | Required information |
| --- | --- |
| Recommended OS | Variant, architecture, image release/date, exact filename and versioned upstream URL, supported Pi model. |
| Integrity | SHA-256 from the authenticated upstream release, recorded with qualification evidence. |
| Compatibility | Tested kernel/firmware, libcamera/IPA, trigger overlays and compatible Nuvo release/channel. |
| Flash instructions | Select the downloaded image in Raspberry Pi Imager; configure username, hostname, network and SSH; write the intended card and boot. |
| Install instructions | Authentic released script, verification method, supported flags and expected endpoint. |

Use an immutable image selection, not an alias that silently advances to a new release. Existing users on a supported OS skip flashing. The script does not reimage storage or upgrade the OS into compatibility. Desktop editions can be supported after equivalent testing, using the same terminal script; Lite is the default because Nuvo does not need a desktop.

The [official downloads](https://www.raspberrypi.com/software/operating-systems/) and [Imager instructions](https://www.raspberrypi.com/documentation/computers/getting-started.html) provide the upstream image and flashing/customization route. On 19 October 2026 the current 64-bit Trixie download lists kernel 6.18; the [September clean-board record](../pi5-byo-setup-prerequisites.md) records `6.12.75+rpt-rpi-2712`. An OS family name alone therefore does not establish compatibility. This specification defines the image policy, not a newly tested image.

## Entry paths

| Current state | Action |
| --- | --- |
| Supported OS and local terminal or SSH | Obtain the authentic released script and run it on the Pi. Reuse its existing network. |
| Blank SD card or unsupported OS | Flash the exact recommended base image, configure networking/SSH, boot, then run the same script. |
| Pi on Wi-Fi but no usable shell | Establish administrator terminal/SSH access first. Knowing its IP does not authorize a website to install software. |
| Replacement SD for an existing Nuvo | Boot the supported base, run the script and supply the trusted recovery/bundle input in the terminal. Use web recovery only if that work was explicitly deferred or could not yet be completed. |
| Existing Nuvo installation | Detect current state, prefill existing values and offer preserve/repair or explicit reconfiguration. Retain valid identity, ownership and calibration. |

Run the script on the Pi, not the laptop used to read the documentation. A local terminal on a desktop OS and an SSH terminal use the same flow.

## One terminal installer, two input modes

Proposed artifact: **`install-nuvo-depth.sh`**. The script and its published endpoint do not exist yet; these commands specify its future interface after downloading and verifying the release artifact:

```text
sudo bash ./install-nuvo-depth.sh

# Production: use the versioned factory defaults without selection prompts.
sudo bash ./install-nuvo-depth.sh --factory

# Optional: permit an automatic reboot if required by detected changes.
sudo bash ./install-nuvo-depth.sh --reboot

# Optional: inspect checks and the proposed changes without applying them.
sudo bash ./install-nuvo-depth.sh --dry-run
```

Normal execution collects required choices, displays the resolved installation/network/ownership plan and asks once to apply it. `sudo` may request the OS password. After approval, package installation and provisioning proceed without repetitive per-package prompts. Unsupported environments or unresolved resource/destructive conflicts produce a precise error and remediation. A noninteractive invocation without `--factory` must not silently select production behavior; report that selections or an explicitly supported complete input file are required.

The normal terminal flow asks whether to restart now when a restart is needed. `--reboot` preselects a permitted automatic restart after successful preparation; factory mode has no questions and, without that flag, reports any pending restart for the production runner. Never restart when unnecessary. No network switch or restart happens while package installation is still in progress.

### Required terminal questions

| Input | Normal behavior |
| --- | --- |
| Network access after installation | **Use current network** is the default. The alternate **Set up network after installation** arms a protected temporary Nuvo AP for later network configuration. Do not request an existing Wi-Fi password again. |
| Device identity | Reuse a valid existing identity. Allow a display name; preserve the OS hostname unless an explicit hostname change is selected and reflected in endpoint/certificate preparation. |
| Hardware/head | Prefill detected supported hardware; ask whether this is a Nuvo head, a restored unit or a custom assembly when needed to select the correct profile. |
| Recovery/calibration input | Ask for a trusted backup or signed bundle path if required. Validate it now. Do not ask again when valid state is already installed. A custom assembly that still needs optical calibration remains pending rather than pretending a prompt can supply measured calibration. |
| Camera mapping/orientation | Ask only when supported detection and supplied identity/profile data are insufficient. Reject incompatible combinations before activation. |
| Ownership | On an unclaimed user installation, offer to establish the local Nuvo owner credential now through masked input, or explicitly defer claiming using a bounded setup capability. Preserve an existing owner; do not prompt to overwrite it during repair. This is a Nuvo credential, not the user's OS account password. |
| Startup and operating preset | Offer the release's supported default profile and start-at-boot behavior; preserve existing settings. Custom tuning does not become a compulsory questionnaire. |
| Final action | Summarize changes, selected access route, owner/calibration state and any restart. Confirm before applying or disconnecting the current session. |

Collect all required installer inputs in the terminal. If something genuinely requires later hardware work, user action or customer Wi-Fi that was deliberately deferred, record that pending state explicitly. Do not duplicate the same required questionnaire in the web UI.

### Network handoff

**Use current network:** preserve the connection and report the actual console endpoint. The script does not create an AP simply because setup or ownership is incomplete.

**Set up network after installation:** retain the installation-time network until downloads, package work, configuration and the resumable worker are complete. Generate a unique AP password, print the SSID, local setup URL and applicable ownership instructions through the authorized terminal, then perform the approved switch or arm it for the chosen reboot. Warn in that final action that a single-radio Wi-Fi SSH connection will disconnect. Preserve the previous connection for activation-failure recovery. Subsequent AP-to-station configuration follows the shared transaction/confirmation/rollback rules.

The deferred-network choice is an explicit AP policy, not an instruction to infer factory origin or reset ownership. If the owner was created in the terminal, the web UI uses that owner's login while configuring the deferred network.

### Factory defaults

`--factory` builds the same resolved plan from a versioned manufacturing preset and available per-unit data, with no selection/confirmation prompts. It does not fork package lists or provisioning logic. Proposed defaults are:

- The qualified software cohort and supported Nuvo head/platform profile, with normal start-at-boot behavior once applicable checks pass.
- Setup AP armed for the customer handoff; no assumed customer SSID/password and no manufacturer network recorded as the customer network.
- Customer ownership left unclaimed, with fresh unique per-unit identity/keys, AP credentials and bounded factory pairing material.
- Matching accepted calibration restored/imported when available; missing calibration or electrical/functional evidence remains an explicit production-pending state.
- No silent restart unless the production runner supplies `--reboot`; no skipped compatibility, signature, camera or release checks.

Factory mode can consume a manufacturing record/validated bundle supplied by the production runner without asking an operator to type it. It does not invent missing per-unit evidence, grant factory-signing authority or mark a unit shipping-ready just because installation completed. Credentials and private keys are never shared defaults or embedded in a cloned base image.

Rerunning factory mode on a partial unclaimed factory installation preserves its unit identity and progress. Running it on an owned or conflicting customer installation fails with an explicit reset/reprovisioning requirement rather than overwriting user state. Factory defaults are input automation, not a reset flag.

Release instructions must supply a real HTTPS endpoint and artifact verification method. Download before execution so the script can be reviewed and authenticated; do not publish fictional URLs or execute an unverified network stream as root. The script may use installed helpers, but users do not first install a separate setup application.

| Stage | Automatic work | Result reported in terminal |
| --- | --- | --- |
| Check | Verify Pi, arm64 OS/ABI, supported release, package health, disk space, network, boot layout and relevant camera/GPIO conflicts. | Detected environment, compatible Nuvo release, blockers and precise next actions. |
| Collect/resolve inputs | Prompt for missing required choices, or use the factory preset and per-unit inputs. Validate and confirm the normal-user plan. | Selected network route, head/calibration/ownership state and final actions. |
| Resolve | Authenticate release metadata and simulate the APT changes for the complete runtime cohort. | Versions, download/storage estimate and necessary platform changes. |
| Install | Enroll the scoped repository key/source and install `nuvo-depth` with its dependencies. | Stage progress and a credential-free log path. |
| Prepare | Back up and idempotently reconcile Nuvo-owned configuration, permissions, identity and known supported camera/trigger settings. | Applied changes, deferred hardware facts and actual reboot reason. |
| Publish access | Persist the approved access policy independently of origin; preserve existing valid state or apply explicitly selected changes. Start shared setup/control services and persist post-reboot work. | Endpoint or future AP details, trust information and pending state. |
| Resume/check | Run bounded remaining checks after reboot through an installed worker, independent of the original shell. | Durable status on reconnect; no repeated initial installation. |
| Handoff | Expose the console using the configured owner login, or bounded enrollment if claiming was deferred. | URL, owner/setup status and, only when needed, short-lived pairing material or its post-reboot reissue command. |

Support a read-only status command/flag and explicit reissue of short-lived pairing material for the authorized terminal user without repeating the install questions. A code printed before reboot must not be assumed valid indefinitely afterward; issue it after restart or provide deliberate reissue. Existing owned installations keep owner authentication.

Long-running package steps and their journal must survive SSH disconnect. Use trusted installed worker/service code rather than depending on a fragile foreground shell or later executing a mutable download in a user's home directory as root.

## Complete package installation

Use the existing **`nuvo-depth` metapackage**. Its exact-version dependencies in [debian/control](../../debian/control) install the full supported runtime cohort:

| Package | Purpose |
| --- | --- |
| `nuvo-depth-core` | Native camera/depth runtime and local frame services. |
| `nuvo-depth-platform-pi5` | Pi 5 platform integration. |
| `nuvo-depth-gateway` | HTTPS API and web console. |
| `libnuvo-depth1` | C client decoder runtime. |
| `python3-nuvo-depth` | Python client and recording primitives. |
| `nuvo-depth-calibration` | Calibration validation, activation and workflow tools. |
| `nuvo-depth-tools` | Identity, platform preparation and diagnostics. |

APT resolves runtime libraries. Users do not compile source, select component packages or install development headers, ROS, Hailo or unrelated AI assets. C/C++ development headers remain optional; factory signing authority is not a customer installation dependency.

Publish an opt-in signed Nuvo repository with immutable packages and a supported OS/ABI manifest. Existing staging tools do not establish that the public feed exists; see the [publication roadmap](../debian-archive-roadmap.md). Authenticate the initial script/key distribution, then use a dedicated `Signed-By` keyring and verified repository metadata/package hashes. [Debian apt-secure](https://manpages.debian.org/trixie/apt/apt-secure.8.en.html) describes that chain; local `.deb` files are not automatically authenticated as individually signed packages.

An offline option must include authenticated script/repository metadata, the complete dependency pool and compatible base-OS manifest. A stock OS image without Nuvo packages is not itself an offline Nuvo installer. The ordinary script needs download access during installation; normal device operation does not require internet.

## Web console after terminal setup

| Terminal script collects/applies | Web UI displays or completes |
| --- | --- |
| OS checks, dependencies, service accounts, permissions and service installation. | Owner login if configured in the terminal; bounded claiming only if explicitly deferred or factory-unclaimed. |
| Existing state, required user selections, defaults, backups and reboot/resume. | Saved device settings and optional later edits; no repeated first-use questionnaire. |
| Head/camera identification and any required trusted recovery/bundle input. | Verified association/status or explicitly pending restoration that could not be completed earlier. |
| Supported mapping, orientation, operating preset and platform preparation. | Actual camera/trigger status and any later repair or deliberately deferred hardware work. |
| Configuration/calibration validation and bounded health evidence. | Accurate calibration authority and an advanced calibration workflow if measurements are still needed. |
| Endpoint, public trust information and installation status. | Real first image, **Show depth**, and optional output/SDK choices. |

If **Use current network** was selected, do not request its password or show compulsory network setup. If **Set up network after installation** was selected, the AP network page collects the deliberately deferred destination SSID/password. Reuse all valid facts already collected in the terminal; optional preferences must not block the first useful image.

### Reachability before camera readiness

The setup endpoint must work while cameras are missing, calibration mismatches, hardware details are pending or a reboot is required. Show these states explicitly and gate capture/trigger activation until the applicable checks pass.

The current [gateway unit](../../deployment/systemd/nuvo-gateway.service) and [Nuvo target](../../deployment/systemd/nuvo-depth.target) request the native daemon. Provide a setup mode/service arrangement that does not implicitly start unconfigured capture just to serve the first-launch page. Qualify gateway initialization/status/setup APIs without a live frame source. Missing prerequisites cannot be handled only behind an inaccessible camera UI.

Web configuration uses a narrow privileged control API; the gateway does not become root or gain arbitrary shell/package execution. If remaining hardware details require further platform writes or a reboot, show and apply that necessary action through the controlled backend. Normal supported setup should not require manual boot-file edits.

## Preparation and preservation requirements

| Area | Required behavior |
| --- | --- |
| OS | Enforce the published supported image/runtime range before platform writes. Do not silently upgrade/downgrade the OS or add global package holds to force a match. |
| APT | Detect broken dpkg state, locks and low space. Do not delete lock files or kill unrelated transactions. Reject a plan that removes unrelated software or requires an unsupported transition. |
| Cameras | Identify intended IMX296 cameras using libcamera/media information. Two `/dev/video*` nodes are insufficient. Missing/ambiguous hardware becomes a pending setup state or a blocker for unsafe changes. |
| Resource owners | Report camera/GPIO conflicts and specific remediation. Do not disable unrelated services automatically. |
| Boot files | Resolve includes/conditional sections, preserve prior boot/module policy, write correctly scoped Nuvo-owned blocks and calculate real reboot requirements. Reruns must not duplicate changes. |
| Trigger | Prepare approved IMX296 mode and RP1 PIO PWM on GPIO17 only when the hardware/profile is established. Verify overlays/tools and avoid the fan PWM controller. Physical XTR wiring follows approved assembly instructions. |
| Permissions | Use packaged accounts and intended video/render/GPIO access; do not run the runtime as root or make devices world-writable. |
| Runtime configuration | Reconcile daemon, stereo, exposure and encoding settings and run `depthd --check-config` before capture activation. Current rectification defaults to `cpu_opencv`; an obsolete mandatory GPU path is not the baseline. |
| Existing state | Preserve Wi-Fi, SSH, hostname, unrelated files/services, valid identity, ownership and calibration unless a specific supported change was selected and approved. An intentional AP handoff may disconnect the current Wi-Fi session only after installation finishes. Rerunning is not resetting. |

Package installation alone continues to avoid boot mutation and automatic camera startup. Running the automatic script is the explicit provisioning action for its documented scope; direct package installation remains available to experts.

## Replacement SD, identity and calibration

[nuvoctl](../../tools/nuvoctl.py) generates a UUID when identity is missing. [The runtime](../../apps/depthd_main.cpp) checks calibration `unit_id` against that UUID and validates camera bindings. A fresh card can therefore reject a valid bundle from the same physical assembly.

- **Repair/reinstall:** preserve and validate existing identity/calibration automatically.
- **Replacement card for the same Nuvo:** collect a trusted backup or supplied recovery record in the terminal and validate signatures/assembly bindings. Web recovery handles explicitly deferred or unresolved restoration. Define whether encrypted private keys are recovered or regenerated with explicit re-enrollment.
- **Nuvo head on another Pi:** use an authorized provisioning/rebinding path; do not edit a signed bundle or remove runtime identity checks.
- **Custom assembly:** create its own identity and geometry/calibration. Do not present a nominal/reference 100 mm bundle as factory-valid for that rig.

Recoverable head identity and signed calibration delivery should be independent of replaceable OS credentials, but the current UUID binding requires a defined migration/restoration policy. A newly generated UUID must not silently imply an established factory head identity. Keep the web setup available while the association is pending. A serial QR or media path alone is not cryptographic hardware attestation.

Restoring head identity/calibration does not automatically restore the former SD card's network/bootstrap policy or consumed ownership capabilities. A replacement card uses the network option selected in this installation, independently of the restored head's factory origin.

Report **Installed — configuration required** or **Cameras working — calibration required** as appropriate. Provisional metric results remain explicitly engineering-only under release policy. Installation does not automatically enable today's `--allow-provisional-depth` engineering override for retail operation.

## Owner enrollment and HTTPS

The authenticated local/SSH administrator can establish the first owner's Nuvo credential directly using masked terminal input through the shared ownership backend. Do not persist or log the plaintext credential. If claiming is deferred, issue a bounded short-lived code/handoff instead of requiring the 64-character bearer token. Factory mode always leaves customer ownership unclaimed. Report the actual endpoint and public certificate fingerprint; secrets belong in authorized terminal/manufacturing outputs rather than logs. Permit deliberate capability reissue after reboot/expiry and preserve ownership during repair.

If ownership was established in the terminal, the web UI uses that owner's login and does not require claiming again. If ownership was explicitly deferred, validate the bounded capability before accepting the initial owner. Being the first LAN browser to arrive is insufficient authority. Export a verified endpoint/public-certificate/identity profile for SDK use, with separate scoped machine credentials.

Pairing/HTTPS retain the shared requirements in [Use case 1](user-flow-use-case-1.md). A sudo-authorized install does not make a self-signed certificate trusted by a remote browser. Provide the supported identity-verified trust path or the qualified public-certificate/discovery design. Report trust failures separately from installation and camera failures.

## Reboot, failure and repair

Persist states such as `checked`, `packages_installed`, `platform_prepared`, `reboot_required`, `web_setup_ready`, `configuration_pending`, `hardware_verified`, `ready` and `failed`. Terminal status and the web UI read the same record. Web readiness does not imply calibrated depth.

- Normal mode offers restart now/later when needed; `--reboot` preselects now. Factory mode does not prompt and reports a pending restart unless `--reboot` was supplied. Persist work before restart; never loop reboots or restart unnecessarily.
- SSH disconnect does not corrupt managed package work. On reconnect, show durable status or a precise recovery action. Rerunning reconciles completed stages.
- Back up boot/module/service state and record offline boot recovery. A service cannot repair a Pi that never boots.
- Stop failed capture/trigger safely and retain the setup/control route where the OS remains reachable.
- Roll back tracked Nuvo changes only; detect intervening edits rather than overwrite them with a stale whole-file backup. Package changes do not provide atomic rollback of the entire OS.
- Add explicit command-line repair/removal operations when implemented. Ordinary removal preserves device data and shared dependencies; purge is separate.
- Updates use verified cohorts and explicit migrations; they are not factory resets or requests to reconfigure Wi-Fi.

## Acceptance scenarios

| ID | Requirement / evidence |
| --- | --- |
| UF2-01 | One script works locally and over SSH, including Lite. Normal mode collects required choices in terminal prompts; `--factory` uses the preset without selections. No Nuvo desktop application is required. |
| UF2-02 | Current networking is the default and unrelated state is preserved. An explicitly selected AP activates only after installation/preparation and approved handoff; it never cuts off its own download/package steps. |
| UF2-03 | One invocation installs the complete exact-version runtime cohort and dependencies without source compilation or manual package selection. |
| UF2-04 | Unsupported model, 32-bit userspace and incompatible OS/ABI fail before platform writes; no hidden OS transition occurs. |
| UF2-05 | Missing/occupied/ambiguous cameras have actionable outcomes. Setup stays reachable with pending hardware; video-node count cannot pass the camera gate. |
| UF2-06 | Reboot is explicit through the flag or a later user command, only when needed, with bounded resume and idempotent reruns. |
| UF2-07 | Fresh install, repair, upgrade and replacement-card paths retain correct identity/calibration checks. Mismatches are not bypassed or relabeled. |
| UF2-08 | Verification failures, unavailable dependencies, low space and interrupted APT produce specific recovery outcomes without starting an incomplete camera stack. |
| UF2-09 | Fresh paired frames, synchronization, configuration, calibration authority and installed release are checked separately from installation. Software checks do not certify wiring or metric accuracy. |
| UF2-10 | Owner creation is authorized through terminal administration or the applicable deferred/factory capability. Pairing capabilities expire; owner credentials and secrets avoid plaintext storage/logs, and browser trust follows the shared policy. |
| UF2-11 | The terminal collects required detected/missing head, recovery, owner and profile information. The web UI reuses it and asks only for explicitly deferred details or genuinely incomplete work. |
| UF2-12 | Offline bundles contain the complete authenticated dependency set for their supported base; a stock OS image is not called an offline Nuvo installer. |
| UF2-13 | The download page publishes an exact tested base image, architecture, SHA-256 and runtime compatibility. A moving latest URL cannot redefine support. |
| UF2-14 | Use the same setup/status/ownership backend and serialized mutations for prompted/factory inputs. AP policy comes from the selected network option or factory preset, never from unclaimed status or hardware origin alone. |
| UF2-15 | `--factory` skips prompts, not validation. Unit credentials remain unique; missing acceptance/calibration evidence blocks shipping readiness; existing customer ownership cannot be silently reset. |

Measure installation, reboot, web setup, first image and first accepted depth separately. Flashing and custom-rig optical calibration have their own durations; no unmeasured single-command timing claim is made.

## Implementation baseline

| Existing source | Reusable capability | Remaining work |
| --- | --- | --- |
| [Debian control](../../debian/control) and [rules](../../debian/rules) | Complete cohort; units install without automatic enable/start. | Released script, dependency audit and compatible public packages. |
| [nuvoctl](../../tools/nuvoctl.py) | Platform checks, managed boot block/backup, module policy and change report. | Effective boot parsing, accurate reboot detection, expanded checks, durable orchestration and rollback. Substring/video-node checks are incomplete. |
| [Trigger helper](../../tools/pi/nuvo_trigger_prepare.py) and [native unit](../../deployment/systemd/nuvo-depthd-native.service) | PIO preparation, restricted runtime, validation and cleanup. | Bounded installation checks, pending-hardware gating and recovery. |
| [Gateway postinst](../../debian/nuvo-depth-gateway.postinst) and [gateway unit](../../deployment/systemd/nuvo-gateway.service) | Identity/certificate provisioning and web service. | Capture-independent setup, first-launch state/API and short enrollment. |
| [Calibration tools](../../tools/calibration/nuvo_calibration.py) | Signed factory verification and protected activation. | Head identity recovery/rebinding and first-launch restore/import. |
| [APT staging](../../tools/release/stage_apt_repository.py) and [publication notes](../../deployment/apt/README.md) | Signed repository staging and offline direction. | Qualified image manifest, initial artifact authentication, published feeds and offline pools. |

The [BYO prerequisites](../pi5-byo-setup-prerequisites.md) and [deployment record](../pi5-full-stack-deployment-20260915.md) contain engineering evidence; some older prose retains obsolete required-GPU, camera-index and one-time-token descriptions. Current source/configuration determines the baseline. This revision produces no runtime code, installation script or OS image.

## Revision history

- **0.4 — 19 October 2026:** normal script collects required information in terminal selections, including current-network/deferred-AP choice; `--factory` applies noninteractive production defaults through the same backend. Web setup reuses completed answers.
- **0.3 — 19 October 2026:** bind both entry paths to a shared setup contract; specify persistent provisioning policy, AP eligibility and preservation across reruns/SD recovery.
- **0.2 — 19 October 2026:** automatic terminal script replaces desktop/text setup applications; first-launch web UI collects remaining details; exact official base-image distribution policy added.
- **0.1 — 19 October 2026:** initial user-owned Pi/SD specification.
