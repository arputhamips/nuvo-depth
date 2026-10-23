# Calibration readiness and existing-bundle import

This reference describes public preview beta 8. The maintained installer selects
this approved release. Older beta 6 does not include these `nuvoctl calibration`
commands; upgrade within the installed profile to use them.

Calibration describes the geometry and its declared validation. Camera/service
health describes whether capture is working now. An accepted bundle can coexist
with a disconnected camera, and a healthy preview can have unverified distance
accuracy. Neither a passing bundle nor a fresh frame proves production readiness.

| State | Meaning | Next action |
| --- | --- | --- |
| Calibration accepted | A compatible per-unit bundle passed its declared validation | Inspect its details and verify fresh output; check camera/service health separately |
| Preview enabled | Provisional output is enabled; distance accuracy remains unverified | Continue evaluation or import an accepted bundle |
| Calibration needed | Capture waits for calibration or an explicit preview selection | Import a compatible bundle or deliberately enable preview |
| Status unavailable/error | The selected bundle, effective policy or configuration could not be verified | Read the specific problem, inspect service logs and repair that cause |

Overview, the Calibration page, installer completion, `nuvoctl status` and
`nuvoctl calibration status` use the same readiness definition. They inspect the
active bundle, device/camera binding, native geometry validation, effective
configuration and effective systemd capture policy. Saved installer choices do
not establish readiness. A fresh running daemon must report the selected hash
and authority; stale/unavailable daemon health remains visible separately.

## Choose before enabling capture

The additive readiness object retains `schema_version`, `state`, `label`,
`message`, `next_action`, `accepted`, `preview_enabled`, `capture_enabled`,
`bundle_id` and `loaded`. It additionally exposes `authority`, `source`,
`calibration_sha256`, device/camera/profile `bindings`, stable `reason`,
`runtime_verification` and separate `camera_service`. Null means unobserved.
`runtime_verification.state` distinguishes unverified, daemon_loaded and
fresh_frames_verified. A verification receipt applies only to the same active
daemon run/hash with capture enabled; stale health cannot establish new output.
Clients must tolerate additive fields and use `state` rather than parsing labels.

The installer explains three choices: preserve/use compatible existing
calibration, deliberately enable provisional preview, or leave capture disabled.
Fresh beta 8 minimal installations on the supported head enable provisional
preview, with an unverified-distance notice. `--defer-capture` leaves a new minimal
installation disabled; upgrades preserve the existing policy, including implicit
older preview. Full appliance preview remains an explicit opt-in. In either
profile you can later choose:

```bash
sudo nuvoctl calibration status
sudo nuvoctl calibration preview --enable
# Later, disable capture:
sudo nuvoctl calibration preview --disable
```

Preview uses the checked-in provisional seed. It never promotes the seed to
per-unit validation. On the unchanged hardware the default geometry reuses beta 5
source bundle `8adf4c8103f1fb7f2811a7d483669cc99c8c7132902700ebb3fd464d19158cf2`;
its seed/provenance remain in `config/calibration/nuvo_depth_100mm/`.
Existing validated calibration is preserved. A software-version change alone
does not require recalibration.

## Obtain and inspect a compatible bundle

Use the original complete per-unit bundle supplied with your camera or exported
from its passing full calibration workflow. Ask your camera supplier/operator
for that bundle if you only have YAML files. Copy the extracted directory to the
Pi. It must contain `manifest.json`, `front_left.yaml`, `front_right.yaml` and
`validation.json`. Keep a separate copy of the original evidence.

The bundle must identify this device UUID, the correct left/right stable camera
IDs, capture profile and geometry, with passing declared validation and matching
content hashes. Factory bundles also require an Ed25519 signature from an
already trusted key. A user bundle is accepted on its declared validation and
identity checks, not a factory signature. Import does not install trust keys,
rewrite unit IDs, relabel provisional files or relax validation gates.

If identity changed during an earlier installation, do not edit the manifest to
silence a mismatch. Recover the correct bundle/identity history with the camera
operator. Existing legacy validated YAML remains preserved, but readiness reports
that its current per-unit bundle binding cannot be verified.

## Dry-run, import and confirm

Both package profiles support the same commands; the minimal package needs no
calibration solver, NumPy or Python OpenCV.

```bash
sudo nuvoctl calibration status
sudo nuvoctl calibration import /path/to/bundle --dry-run
sudo nuvoctl calibration import /path/to/bundle
sudo nuvoctl calibration status
sudo nuvoctl status
```

Dry-run checks the complete private copy without changing service files,
selection or capture policy. Review the previous/new IDs, authority, unit and
capture change. Import prints that plan before activation, archives the checked
bundle under its content ID, and transactionally selects it with capture enabled.
Accepted active bundles take precedence over the seed/legacy fallback, so a
minimal installation needs no manual systemd command override.

Success requires a new running daemon instance, the expected calibration hash
and authority, and two advancing associated depth/validity frames carrying that
hash and daemon run ID. Numeric accuracy is still a separate known-distance
evaluation. Camera disconnection, service failure or metadata disagreement makes
activation fail and restores the previous selection and capture policy. An
interrupted transaction is recovered before capture starts or another import
proceeds. A failed recovery stays recorded and reports the specific problem.

```bash
sudo nuvoctl calibration rollback
sudo nuvoctl calibration status
sudo journalctl -u nuvo-depthd-native.service -b --no-pager -n 60
```

Rollback restores the previous committed selection and its policy, including
disabled capture before the first import. An older validated legacy fallback is
checked against its previously loaded hash when restored. Factory selection has
precedence; a user import cannot silently replace it. Use
`rollback --namespace factory` only when deliberately reverting a factory import.
No previous committed selection produces an actionable error.

For a minimal sample, run `sudo nuvoctl depth capture --output check.f32` and inspect
`check.f32.json` alongside the validity bytes. On the full appliance use **Show
first depth** and save the NUV1 sample, or use the Python first-use helper. Inspect
the original frame identity, `calibration_sha256`, `calibration_authority` and valid
pixels; file creation alone does not establish calibration acceptance.

## When the full calibration workflow is needed

Use the full workflow when no compatible passing per-unit bundle exists, or
optics, focus, camera pose, baseline, sensor readout/crop/orientation or image grid
have changed. It captures synchronized raw observations and solves/validates
geometry. Import only applies an existing result. The Calibration page keeps
readiness and the next action prominent; geometry and qualification parameters
are under **Advanced geometry and qualification parameters**.

