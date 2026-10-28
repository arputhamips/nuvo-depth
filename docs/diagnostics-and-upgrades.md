# Diagnostics and maintenance

These commands are included in public preview beta 9. The approved public
installer follows `deployment/public-release.json`, presently beta 9. A development
version or draft never advances that public reference automatically.

```bash
sudo nuvoctl diagnostics
sudo nuvoctl diagnostics --json
sudo nuvoctl diagnostics --output support-report.json
```

Diagnostics only reads status. It does not repair services, rewrite network
configuration, enable preview, upload data or change calibration. An existing
output file is refused. The full console's Diagnostics page can refresh, copy
or download the same allowlisted report; minimal has only the CLI.

Schema version 1 has `report_type=nuvo-depth-support`, UTC `generated_at`, profile
(`appliance`, `minimal`, `unknown`), checks, installed versions, calibration,
output, upgrade and privacy. Each check contains stable `id`, `status` (`pass`,
`fail`, `warning`, `unknown`), a controlled reason and next action. Clients must
allow future additional check IDs and use IDs instead of parsing prose.

| Stable check | Meaning |
| --- | --- |
| platform.os / platform.kernel | Supported board/OS and qualified package kernel |
| capture.cameras / capture.trigger / capture.service | Camera pair, trigger mode and independent service state |
| calibration.readiness | Declared calibration or provisional/needed/unavailable policy |
| output.depth / output.monitoring | Demand-driven processing and optional encoder availability |
| network.budget | Measured budget when available; otherwise explicitly unknown; absent by design on minimal |
| software.versions | Installed Nuvo package versions |

Only checked enums, version strings, calibration SHA-256 and bounded numeric
observations leave the export builder. Raw logs, arbitrary configuration, error
bodies, admin tokens, cookies, passwords, private keys, setup secrets and network
confirmation links are excluded. Null means unavailable. No identity, private
setup files or arbitrary source strings are copied. Review the generated report
before sharing it. Retain raw logs privately for a deliberate separate review.

An idle matcher is healthy when nobody requests depth. Disabled monitoring does
not imply failed raw depth. Accepted calibration and camera/service health are
separate: request advancing depth/validity output to check actual capture.

## Upgrade and interrupted-operation recovery

The Software update page shows the approval reference bundled with its release,
which can precede that release's publication. Use the maintained GitHub command
below for the current approval. It resolves beta 9 and verifies its signed
cohort; an implicit downgrade is refused. Installed-version reporting remains
accurate.


Read the selected release's platform prerequisites first. Re-run the maintained
GitHub bootstrap for the installed profile, or pin the approved release explicitly
as shown in Software update. That page displays installed versions and the
bundled public approval; it does not query live release availability or perform
a browser update. An internal candidate can be newer than the public reference.
An implicit downgrade and full/minimal conflict are refused.

For a legacy hand-configured full installation, complete the
[legacy upgrade preflight](installer-reference.md#legacy-hand-configured-full-installations)
before running the bootstrap. A calibration path through the mutable runtime
`current` link needs an explicit preservation review.

```bash
# Full appliance (use the minimal script for an existing minimal installation):
curl -fsSL https://raw.githubusercontent.com/arputhamips/nuvo-depth/main/install-nuvo-depth.sh | sudo bash
sudo dpkg --audit
sudo nuvoctl calibration status
```

Supported same-profile upgrades preserve `/var/lib/nuvo-depth` identity,
recordings and calibration, `/etc/nuvo-depth` user settings and existing network
choice. The minimal installer migrates the old effective service policy before replacing
the unit: implicit beta 6 preview remains preview, a stopped preview remains
preview, validated per-unit selection remains selected, and deliberate disabled
capture stays disabled. Full preview remains opt-in. A software version change
does not require recalibration. New shared defaults retain provisional authority.

If download/verification fails, retry the same exact signed release; partial or
tampered bundles are not accepted. If package work was interrupted, inspect
`sudo dpkg --audit` and the install error, complete the named OS/package repair,
then retry the same cohort and check fresh output. A required reboot remains
required. Do not mix individual package versions or force a downgrade. No package
downgrade/rollback transition is advertised as qualified. Calibration rollback
uses its supported separate transaction and retains integrity/identity rules.

If `dpkg --audit` specifically reports packages that are unpacked but not yet
configured, complete their pending configuration before retrying the installer:

```bash
sudo dpkg --configure -a
sudo dpkg --audit
```

Continue only when the audit is empty. If configuration fails, retain its exact
error and resolve the named dependency, filesystem, or service problem first;
repeated installer attempts do not repair a broken OS package transaction.

Candidate lifecycle results and their limits must accompany the exact artifact
manifest. A host fixture or Debian userspace container cannot establish physical
Pi reboot, Wi-Fi, smaller RAM SKU or new-user acceptance.
