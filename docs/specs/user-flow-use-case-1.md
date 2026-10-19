# User flow — Use case 1: first setup of an assembled Nuvo device

**Specification:** UF-01 · **Version:** 0.3 · **Updated:** 19 October 2026  
**Status:** Product design baseline with initial factory provisioning, protected hotspot and short-code ownership implemented. Hardware qualification and broader discovery/recovery features remain pending; see [implementation and limits](../install.md).

## User and outcome

The user has an assembled Nuvo camera with its supplied software image. They have a phone or computer and can connect power. Nuvo does not know their Wi-Fi credentials. Ethernet may be unavailable. The user must reach setup, establish ownership, and see a real camera image without a terminal or entry of a long administrative token.

Success means the selected connection works, the correct device is paired, a fresh camera frame is received, and calibration authority is stated accurately. Package installation, camera assembly and manufacturing calibration have already happened before this journey.

Use [Use case 2](user-flow-use-case-2.md) when the user supplies the Pi, replaces the SD card or installs Nuvo onto an existing OS.

Both entry paths follow the [shared setup contract](shared-setup-contract.md). Production runs the same installer as UF-02 with `--factory`, which supplies the manufacturing defaults without terminal selections. Customer ownership remains unclaimed and customer network setup is deferred. AP activation follows the explicitly armed policy and handoff state; unclaimed ownership or factory hardware alone does not activate it.

Normal UF-02 users can also deliberately select deferred setup-AP access without becoming factory units. Factory mode determines the input defaults and customer handoff, not a second runtime or weaker validation.

## Preconditions and supplied material

- The appliance has a supported image, unique identity and keys, a unique setup AP password, and factory setup material. Factory calibration must match the actual unit and camera assembly.
- Factory installation uses `install-nuvo-depth.sh --factory` with the qualified preset and per-unit data. The flag removes repeated selections but does not generate shared credentials, replace per-unit calibration or authorize shipping before acceptance passes.
- The supplied power/cooling arrangement is qualified for the complete appliance. USB data operation is advertised only for an independently qualified accessory and host configuration.
- A private card identifies the unit, gives its setup SSID/password and a Wi-Fi QR, prints a manual local setup URL, and supplies a separate short ownership code.
- A phone is optional: all QR actions have a typed alternative suitable for a laptop.
- A physical Setup/recovery action and visible state indication must be implemented and documented. Their hardware assignment is a design task; GPIO17 is occupied by the camera trigger.

Illustrative card content, not real credentials:

```text
Camera: 7K2M
Setup Wi-Fi: Nuvo-Setup-7K2M
Wi-Fi password: <unique random password>
Setup page: http://192.168.4.1
Pairing code: C6PV-9R4T

No internet on the setup network is normal.
```

The public help QR contains instructions, never a permanent bearer token. The private Wi-Fi QR encodes only that unit's setup network credentials. Unit names, pairing codes and AP passwords must not be derived from one another.

## Main flow

| Step | What the user sees and does | Required device behavior |
| --- | --- | --- |
| 1. Power | Connect the supplied power arrangement. | The factory preset has armed protected setup-AP access for the customer handoff. Start that AP when pending and authorized by the shared policy, or reuse a usable customer connection explicitly configured instead. Production must not leave a bench network masquerading as the customer's network. |
| 2. Connect | Join `Nuvo-Setup-XXXX` by QR or printed password. | Supply a local address and DHCP. Explain that internet is not required and the client should remain connected. |
| 3. Open setup | A captive notice may open; otherwise enter the printed URL. The page identifies the camera. | Serve setup assets locally; no cloud account or CDN is required. The captive popup is optional, not the only entry. |
| 4. Choose connection | Select **Connect to Wi-Fi**, or **Use Ethernet** when an appropriate wired connection is available. | Show nearby SSIDs and allow manual/hidden SSID entry. Existing links are detected independently of internet access. |
| 5. Configure | Enter the chosen Wi-Fi password. | Stage a new connection without deleting the previous usable configuration. Never assume the browser can read the computer's saved Wi-Fi credentials. |
| 6. Prepare handoff | Read **Reconnect this computer or phone to the same Wi-Fi, then open this address**. Select **Switch and continue**. | Display the unique endpoint and recovery instructions before switching the Pi radio from AP to station mode. Do not display a guessed DHCP address as confirmed. |
| 7. Pair | Open the intended endpoint, verify device identity, enter the short setup code, and establish local owner access. | Use the supported HTTPS trust process, enforce the pairing window and attempt limits, and consume the initial claim capability on success. |
| 8. Verify | See a real mono image and device health; choose **Show depth**. | Confirm connectivity on the selected link, close temporary provisioning, and enter the ordinary console. Report real frame freshness and calibration authority. |

Keep the initial preview modest: the current left-mono 5 Hz demand is the starting behavior. Depth preview is an explicit next action. An illustrative image or a merely running service cannot satisfy the success criterion.

## Alternate and recovery flows

**Existing customer Wi-Fi:** retain a usable station connection when current-network access was explicitly configured for the customer and skip AP/credential provisioning. Establish ownership through the same bounded factory proof and HTTPS route. A deliberately armed AP handoff can replace the installation-time connection only after provisioning is complete; unclaimed status alone cannot do so.

**Ethernet:** use the existing local network and the same identity/pairing steps; skip Wi-Fi credential entry. The setup AP can remain another route under the selected policy without taking over the Ethernet LAN. A direct laptop-to-Pi Ethernet cable is not automatically a DHCP network and needs a separately qualified addressing design.

**Qualified USB Ethernet:** enter the same local wizard over the USB network link without asking for home Wi-Fi. This path is conditional on power/data wiring, host drivers, automatic addressing and throughput qualification. It does not imply UVC webcam support.

**Wrong password or failed network handoff:** restore the same setup SSID/password. Preserve a useful failure reason and allow correction without reflashing. Distinguish association failure, no DHCP address, and a client unable to reach an otherwise connected device.

**Interrupted setup:** closing a tab, losing power or rebooting during an unconfirmed connection change must leave a recoverable route. A device service, not browser JavaScript, owns timeout and rollback.

**Owned device loses its router:** attempt normal reconnection. Opening recovery must not silently reopen first ownership claiming. Re-enter setup through an authenticated owner action or the documented physical action.

**No home network:** a deliberate persistent device-AP mode is a later optional mode, subject to HTTPS and performance qualification. It is distinct from leaving the temporary setup network accidentally enabled.

**Network recovery versus ownership reset:** repairing connectivity preserves ownership, calibration, identity and recordings. Ownership reset is a separate physical process with an explicit customer-data and credential policy; factory calibration assets remain protected.

## Required behavior

| ID | Requirement |
| --- | --- |
| UF1-01 | Setup must work with unknown customer Wi-Fi credentials, no Ethernet and no internet. |
| UF1-02 | The AP must have a per-unit random password and at least WPA2/AES. An insecure fallback is not acceptable. |
| UF1-03 | AP and station mode may run sequentially; correctness must not depend on simultaneous operation on one radio. Scanning behavior must be tested on the shipping driver. |
| UF1-04 | Render handoff instructions before disconnecting the AP. Preserve a transaction journal and independently enforced rollback. |
| UF1-05 | A successful authenticated request through the intended new connection confirms the handoff; DHCP alone does not. |
| UF1-06 | The human pairing code is a new bounded protocol, not a shortened permanent API token. Initial proposal: eight random unambiguous base32 characters, case/separator normalization, five failed attempts per window, persistent lockout, and a 10-minute window. |
| UF1-07 | After ownership is established, new browsers use owner authentication or owner-approved pairing. Issue separate strong, revocable SDK credentials. |
| UF1-08 | Pairing secrets, Wi-Fi passwords and permanent machine credentials must not enter support logs, analytics or public URLs. |
| UF1-09 | Network settings, pairing state and calibration authority are separate states. A successful setup does not promote provisional calibration. |
| UF1-10 | Supported connection endpoints must match running listeners and discovery advertisements, including the actual port. |
| UF1-11 | Apply the shared provisioning/AP policy and ownership store. `--factory` supplies defaults through the same plan as normal terminal selections. Reruns, upgrades and calibration restoration do not restart claiming or independently create an AP. |

Proposed timing targets are AP availability within 90 seconds, up to 120 seconds for Wi-Fi association/DHCP and up to 180 seconds for the overall handoff. Validate these values with novices. These new connection deadlines are separate from the current 60-second transport-preference transaction.

## HTTPS and trust boundary

The example HTTP address is an isolated network-bootstrap service bound to the setup interface. It is not an HTTP copy of the administrative gateway. It may provision Wi-Fi inside the physically controlled setup window; it must not expose streams, calibration, firmware administration or the existing bearer token. Unique AP protection, host/origin checks, CSRF controls and restrictive interface/firewall rules are required. Accepting a network password there is an explicit bootstrap trust tradeoff; AP encryption does not provide browser TLS identity.

The full console retains HTTPS. A QR or short code does not make the current self-signed certificate browser-trusted. Support identity-verified private trust/helper enrollment for engineering installations. Prototype per-device vendor-domain certificates and a complete local/LAN resolution, renewal and offline-recovery scheme for browser-first retail. Do not share a wildcard private key across devices or instruct users to bypass arbitrary warnings.

Seamless browser-only retail access remains blocked until that trust/discovery design passes qualification. This specification does not declare it implemented.

## Acceptance scenarios

1. A novice with no Nuvo software, Wi-Fi settings or prior certificate trust reaches local setup with a laptop alone; phone QR setup also works.
2. Wrong Wi-Fi credentials, missing mDNS, absent captive popup and blocked peer traffic produce useful, distinct outcomes and a working recovery route.
3. Reboot or disconnect at each handoff stage does not strand the unit or erase identity/calibration.
4. Multiple nearby cameras remain distinguishable; the wrong card/code cannot claim another unit.
5. Attempt limits survive reboot; successful initial claiming invalidates the factory claim capability.
6. The success screen displays a newly received frame. Engineering/provisional calibration remains visibly distinct from accepted metric calibration.
7. Owned devices reconnect after ordinary outages without reopening ownership claiming.

## Implementation baseline and related evidence

The current repository has strong bearer-token authentication, private HTTPS, transport-preference rollback and a USB ECM skeleton. It does not yet have this complete AP wizard, short-code ownership protocol or retail trust lifecycle. The source review and official product/USB references are preserved in the [18 October connectivity and USB study](../local-research/nuvo-first-use-connectivity-and-usb-power-study-20261018.md). That research file is local and ignored; this specification is the version-controlled product-flow record and contains the flow independently of that study.

Primary workflow references are [Volumio's setup hotspot](https://help.volumio.com/en/articles/82-what-is-the-hotspot-network), [WLED's first connection and Wi-Fi handoff](https://kno.wled.ge/basics/getting-started/), and [GL.iNet's first-time local web setup](https://docs.gl-inet.com/router/en/4/faq/first_time_setup/). USB topology is supported by [Raspberry Pi's gadget guidance](https://www.raspberrypi.com/news/usb-gadget-mode-in-raspberry-pi-os-ssh-over-usb/); Nuvo's exact power/data accessory still needs its own acceptance evidence.

USB power/data selection is a supporting hardware workstream: GPIO power is not intrinsically required for Pi USB-C device mode, but the exact external-power arrangement must pass electrical and host qualification. See the study for accessory candidates and the USB 2.0 bandwidth budget. This flow makes no unmeasured throughput claim.
