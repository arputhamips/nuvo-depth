# Install Nuvo Depth

The terminal installer and manufacturing preset use one provisioning backend.
Normal installation asks questions; `--factory` supplies the same plan from
manufacturing defaults. The installed gateway also accepts the owner password
or a short single-use setup code.

**Current status:** GitHub ARM64 package builds and Ed25519 signing are configured.
Customer install releases require the platform qualification recorded in their
signed manifest. Use only an explicitly published release. The public source and
release repository is [arputhamips/nuvo-depth](https://github.com/arputhamips/nuvo-depth).

The current prerelease tested for setup is
[v0.1.1-beta.4](https://github.com/arputhamips/nuvo-depth/releases/tag/v0.1.1-beta.4).
Use that exact tag in the commands below. The
[qualification record](release-qualification-20261019.md#uf-01-and-uf-02-physical-setup-acceptance)
distinguishes physical first-use tests from fresh-SD and manufacturing acceptance.
Physical Pi acceptance, USB power/data qualification and shipping approval
remain separate gates.

## Supported machine

Use a Raspberry Pi 5 running the **qualified Raspberry Pi OS Lite 64-bit Trixie
image** listed in the chosen release's signed `manifest.json`. It pins the
upstream `.img.xz` URL, SHA-256, and exact supported `uname -r` values. Flash the
official upstream image using Raspberry Pi Imager; an ISO or separate Nuvo base
image is unnecessary. A compatible existing installation needs no reflash.

The installer checks model, OS, arm64 userspace, release kernel, boot config,
dpkg health, NetworkManager, AP capability and subnet overlap when applicable.
It installs the exact camera-library/IPA/rpicam cohort required by the Pi platform
package; that may upgrade those userspace packages. It does not perform a full
OS/kernel upgrade or repair unrelated broken packages. Use a
unique OS hostname, configured Wi-Fi regulatory country, working internet/APT,
and administrator access before starting. Keep a local terminal or Ethernet
available when deliberately switching a Wi-Fi SSH connection to a hotspot.

## Terminal installation (UF-02)

Open a terminal on the Pi (locally or in an interactive SSH session) and run:

```bash
curl -fsSL https://github.com/arputhamips/nuvo-depth/releases/download/v0.1.1-beta.4/install-nuvo-depth.sh | sudo bash
```

Answer the questions, review the plan, and let the installer finish. Then open
the console URL it prints. You do not need to choose or download Debian packages.
The published script is pinned to its release; `--release` is optional for that
script. Terminal questions and masked passwords continue to work when piped.

The same command can be copied into the product wiki. To use a future release,
update the version in the URL after its qualification and publication.

For a reviewed source checkout, explicitly select the release:

```bash
# Replace vX.Y.Z with an exact published, qualified release tag.
sudo bash ./install-nuvo-depth.sh --release vX.Y.Z
```

To inspect the script before running it:

```bash
VERSION=v0.1.1-beta.4
curl --proto '=https' --proto-redir '=https' -fL \
  "https://github.com/arputhamips/nuvo-depth/releases/download/$VERSION/install-nuvo-depth.sh" \
  -o install-nuvo-depth.sh
less install-nuvo-depth.sh
sudo bash ./install-nuvo-depth.sh
```

The script downloads one installer bundle and checks its Ed25519 signature
before extracting it. It then verifies the inner manifest and installer archive;
the installer checks all package hashes and metadata before APT installs them.
Piped normal setup reconnects input to the controlling terminal; an unattended
session without a terminal receives an explicit error. The signed payload still
names an exact release and refuses moving aliases such as `latest`/`main`.
The initial script and public-key fingerprint
must come through a trusted distribution channel; a key fetched alongside an
untrusted installer does not establish trust.

The questions cover:

1. Keep the current network, or configure Wi-Fi afterward through a protected
   Nuvo hotspot. Keeping the current network is the default. Hotspot setup also
   asks for the country where the Pi will operate, retaining a detected value.
2. Device display name; supported Nuvo head or custom/incomplete hardware.
3. Optional matching calibration bundle and its authority. Factory signatures
   require separately provisioned factory trust keys.
4. Explicit provisional engineering preview, when using the supported head.
   Without accepted calibration or this opt-in, only the console starts.
5. Set an owner password now, or defer ownership using a short setup code.
6. Boot startup and deliberate reboot/hotspot activation. A single summary is
   shown before applying the plan. Hotspot recovery requires boot startup.

The display name is a Nuvo label; it does not change the OS hostname. Custom
hardware receives the console but no assumed IMX296 capture configuration.
Camera mappings, trigger wiring and unsupported geometries require a qualified
configuration outside this initial installer. Default output geometry remains
the packaged supported Nuvo profile.

`--dry-run` checks selections and verifies downloaded packages without installing
them or changing platform/network/service configuration. It still needs network
access and temporarily downloads the release. `--wifi-interface wlan0` and
`--ap-address 192.168.4.1/24` allow an explicit supported AP interface/subnet.

Package work runs in `nuvo-install.service`, independently of the SSH session.
All Nuvo packages must belong to the same signed version/architecture cohort;
APT resolves OS dependencies, reinstalls the explicitly selected Nuvo cohort
on retries, and refuses removals. Implicit Nuvo downgrades
are rejected. Capture preparation uses a backed-up managed boot-config block
and refuses conflicting camera overlays/includes for manual resolution.

## Factory installation (UF-01)

```bash
sudo bash ./install-nuvo-depth.sh --factory --release vX.Y.Z \
  --wifi-country US --calibration-bundle /path/to/this-units-signed-bundle --reboot
```

Forks can override `--repository` and supply their own `--public-key`. Omitting
the calibration bundle leaves calibration pending and capture disabled. The
factory preset never enables provisional preview or claims a customer owner.
Set `--wifi-country` to the unit's actual operating region (US is an example),
or preconfigure it in the OS. Factory mode refuses an unknown country rather
than choosing one. The installer persists the country while preparing the unit.
It retains the station connection until deliberate AP activation after package
work and delivery of setup material have finished.

Factory mode asks no questions. It creates unique per-unit identity, certificate,
AP password and eight-character setup proof (displayed as `ABCD-EFGH`), enables
the setup/console services, and arms the hotspot for the next boot. The same
download, package, configuration and validation path runs in both modes.
`--reboot` permits a required camera reboot; otherwise the operator restarts
deliberately after recording setup material.

For a new unit, run first without a calibration bundle to allocate its identity.
Use `/var/lib/nuvo-depth/identity/device.uuid` in the separate per-unit calibration
process, then rerun factory installation with that unit's accepted signed bundle.
An arbitrary pre-existing legacy identity without factory setup state is refused.

Print the unique AP SSID/password, setup code, hostname, and certificate
fingerprint on the unit's setup card using the root-only material. Secrets are
displayed only to an interactive terminal, never installer service logs.

```bash
sudo nuvo-setup private       # private interactive terminal only
sudo nuvo-setup status        # contains no credentials
```

Run provisioning separately for each unit. Do not clone an already-provisioned
card, its identity, or its credentials. Prepare a unique OS hostname for each
unit before installation. The factory flag preserves an unclaimed factory
installation on retry and refuses an owned/customer installation; it is not a
reset or a calibration/electrical acceptance bypass. Manufacturing acceptance
and evidence must pass before shipment; the setup status never certifies them.

## Protected hotspot and browser handoff

1. Join the unique `Nuvo-Setup-XXXXXX` WPA2 network with its printed password.
   Remain connected when the phone reports no internet.
2. Open `http://192.168.4.1/` (or the explicitly configured AP address). This
   small local page collects the destination SSID and personal Wi-Fi password.
   It uses the protected AP, has no external assets, and provides no internet
   routing. It is not a captive-portal implementation or a UVC/USB setup path.
3. The page provides a confirmation link. Keep it open, reconnect your phone or
   computer to the destination network, and open that link within three minutes.
   Nuvo only commits autoconnection after confirmation arrives through the new
   network while the expected NetworkManager connection is active.
4. If joining or confirmation fails, the same protected hotspot returns with
   the same password and explains the failed handoff on the setup page.
   Interrupted changes, including reboots, follow this
   recovery path. An ordinary outage after confirmed setup does not reopen it.
5. Open `https://HOSTNAME.local:7443/`, verify the self-signed device certificate
   against the printed SHA-256 fingerprint, then enter the owner password or
   short setup code. First claim asks for a permanent owner password only if
   the terminal installer did not already set one.

The initial implementation uses a manually entered SSID, the setup card and an
explicit confirmation link. QR generation, automatic captive-portal detection,
Wi-Fi scanning, enterprise Wi-Fi and physical-button recovery are future work.
Do not promise their availability based on the wider product-flow specs.

Passwords are salted scrypt verifiers in the root-only ownership store. Setup
codes are consumed on success, expire ten minutes after the first attempt,
and are locked for five minutes after five failed attempts. Wi-Fi credentials
are root-only NetworkManager keyfiles, not command-line arguments or logs.
Existing long administrative tokens remain compatible with the gateway/SDK.

## Status and recovery

```bash
sudo python3 tools/nuvo_install.py --status  # reviewed checkout
sudo nuvo-setup status                      # installed helper
sudo journalctl -u nuvo-install
sudo nuvo-setup pair                        # deliberately reissue setup proof
sudo nuvo-setup activate-ap                 # only an already-armed, unconfirmed AP
```

Root job/auth/setup records and cached release files live in
`/var/lib/nuvo-depth-setup/`, outside the capture service's writable data tree.
Identity/calibration remain in `/var/lib/nuvo-depth/`. Preserve both directories
when servicing a unit. A failed install records its state; repair the reported
OS/package issue and rerun the same reviewed release. It preserves identity,
claimed ownership, AP secrets and confirmed handoff state. It does not perform
an automatic package downgrade, uninstall, OS rollback, or customer reset.

If the UI was claimed but the password was never saved, reissue a setup code
from the trusted terminal. Physical reset/recovery is not implemented here.
When starting without calibration, use the terminal's explicit engineering
preview option for calibration capture, then rerun with the accepted bundle
and disable engineering preview for authority-enforced operation.

For a cached bundle, download `nuvo-depth-pi5-arm64.tar.gz` and its `.sig` from
the same release. Verify it using your independently trusted release key before
extracting:

```bash
openssl pkeyutl -verify -pubin -inkey /trusted/release.pub -rawin \
  -in nuvo-depth-pi5-arm64.tar.gz -sigfile nuvo-depth-pi5-arm64.tar.gz.sig
mkdir nuvo-install
tar -xzf nuvo-depth-pi5-arm64.tar.gz -C nuvo-install
cd nuvo-install
sudo bash ./install-nuvo-depth.sh --bundle "$PWD" --public-key /trusted/release.pub
```

This avoids downloading Nuvo packages. OS dependencies still need configured
APT sources or a prepared cache; it is not a fully offline OS provisioning kit.

## Building and publishing releases

See [Release operations](releases.md) for the configured signing key fingerprint,
GitHub environment, ARM64 Trixie build, source archives, platform qualification,
and draft/prerelease/publication commands. The release and factory-calibration
keys are separate trust domains. Modified software may be built and installed
using an owner's own release key and `--public-key` / `--bundle`.

Wi-Fi profiles use the documented
[NetworkManager keyfile format](https://networkmanager.dev/docs/api/latest/nm-settings-keyfile.html).
