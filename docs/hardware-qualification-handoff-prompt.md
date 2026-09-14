# Nuvo Depth Pi 5 hardware qualification handoff

You are validating, not redesigning, the Nuvo Depth Pi 5 stereo appliance.
Work in the supplied repository and connect only to the SSH host and hardware
explicitly provided by the user. Preserve all existing changes.

First read `docs/production-implementation-status.md`,
`qualification/pi5-beta.yaml`, `docs/pi5-depth-100mm-profile.md`, and
`config/pi/README.md`. Confirm the exact Pi model, OS/kernel, libcamera/IPA/Mesa
cohort, camera identities, 100 mm mechanical baseline, trigger wiring, cooling,
power supply, and recovery access. Do not assume that a userspace GPIO pulse
proves electrical safety or sensor exposure timing.

Build with the `pi5-release` CMake preset. Run all tests, then run
`nuvoctl platform preflight` before making any platform change. If preparation
is needed, show the exact proposed changes first; use the opt-in `--apply` only
after the user confirms the target device. Never enable USB gadget mode without
the approved VID/PID, approved power/data arrangement, and explicit electrical
approval.

Execute the qualification plan with
`/usr/lib/nuvo-depth/run_qualification.py`. Collect separate signed or hashed
artifacts for the scope waveform, per-unit calibration, depth accuracy and
coverage, Ethernet TLS throughput, USB electrical and TLS throughput, thermal
soak, package lifecycle, and recovery power-cut tests. Do not manufacture pass
records. Stop at any failed automated gate and preserve its output.

Return the complete evidence directory, exact commands, package hashes, and a
gate table. Recommend release only if `qualification.json` says
`release_authorized: true`; otherwise identify the smallest next action. Never
call the provisional seed calibration factory-valid.
