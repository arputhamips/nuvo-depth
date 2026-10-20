# Manufacturing provisioning

- Internal manufacturing reference for the shared provisioning backend.
- Customer installation: [Install Nuvo Depth](install.md).
- Device identity, credentials and calibration are unique to each unit.

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

