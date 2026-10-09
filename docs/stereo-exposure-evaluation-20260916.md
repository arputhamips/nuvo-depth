# Stereo exposure evaluation and implementation — 2026-09-16

## Outcome

Nuvo Vision One keeps a single exposure/gain authority for both IMX296
cameras and the shared XTR pulse. Schema v1 and all existing API clients remain
compatible. The controller now adds:

- a dynamic shutter ceiling of `min(8000 us, trigger period - 250 us)`, also
  enforced by the capture backend;
- continuous Manual-to-Auto and Auto-configuration transitions using the live
  command instead of stale seed fields;
- two-observation direction confirmation for normal bright/dark corrections,
  while one-sided clipping remains immediate;
- per-camera median and clipping telemetry, stereo median difference,
  controller/settle/confirmation state, limit flags, and the last adjustment
  factor;
- process-side last/minimum/maximum XTR pulse durations;
- a simpler console with basic controls first, advanced Auto and white-balance
  settings collapsed, microsecond/millisecond display, live cadence limit,
  explicit persistence/camera confirmation, and an unsaved-change reset.

The shipping default remains Manual. Auto is suitable for slow scene-light
changes after hardware acceptance; it is not a cure for alternating lighting
or an unstable trigger waveform.

## End-to-end ownership

The control path is:

1. Web console or Python SDK reads/writes the closed schema-v1 exposure object.
2. The authenticated gateway sends an allowlisted request to the root-owned
   system agent.
3. `nuvoctl` validates all 15 fields and atomically replaces
   `/etc/nuvo-depth/exposure.json`.
4. `depthd` detects the changed document before acquiring the next pair,
   preserves the last valid command on invalid input, and applies one command
   to both libcamera request queues and the trigger thread.
5. In Auto, the daemon samples the central 60% of both original ISP Y planes
   before rectification. The control decision therefore cannot be corrupted by
   matcher, depth, encoder, or transport output.
6. Runtime health carries requested settings, per-camera applied libcamera
   metadata, metering/controller state, trigger diagnostics, and capture-pair
   timing to the UI and support tools.

Camera request controls can appear after the pipeline's normal frame delay.
The settle holdoff prevents old frames from causing repeated corrections. Both
cameras remain locked to one requested exposure/gain pair throughout.

## Evidence reviewed

Raspberry Pi documents that IMX296 external-trigger exposure equals the XTR
low width plus approximately 14.26 us, that trigger frequency sets frame rate,
and that fixed camera controls are needed to prevent 3A settling drops:

- [External Trigger on the Global Shutter Camera](https://www.raspberrypi.com/documentation/accessories/camera.html#external-trigger-on-the-global-shutter-camera)

Libcamera specifies manual exposure/gain behavior, metadata reporting,
frame-duration limits, flicker-period controls, and continuity across
manual/auto transitions:

- [libcamera control definitions](https://docs.libcamera.org/master/internal-api/namespacelibcamera_1_1controls.html)

The following primary research was evaluated:

- [Gradient-based Camera Exposure Control for Outdoor Mobile Platforms](https://arxiv.org/abs/1708.07338) — motivates feature-aware exposure and explicit
  multi-camera brightness consistency for stereo/robot vision.
- [Camera Exposure Control for Robust Robot Vision with Noise-Aware Image Quality Assessment](https://arxiv.org/abs/1907.12646) — motivates treating analogue
  gain noise separately from shutter and considering edge/texture quality.
- [Examining Autoexposure for Challenging Scenes](https://openaccess.thecvf.com/content/ICCV2023/html/Ernst_Examining_Autoexposure_for_Challenging_Scenes_ICCV_2023_paper.html) —
  reinforces robust treatment of difficult highlight distributions.

The current median/clipping controller was retained because it is bounded,
deterministic, cheap, and already supported by rig measurements. A gradient,
entropy, learned, or nonlinear optimizer would add tuning and failure modes
without current drive-scene evidence. The evidence instead supported targeted
continuity, damping, safety, telemetry, and UX changes.

## Control law

The controller samples every fourth pixel in the middle 60% of equal-size
left/right 8-bit luma images. It computes a pooled median for control and
separate left/right medians and clipped fractions for diagnosis. The larger
camera clipped fraction governs protection, so a bright region in only one
view cannot be hidden by the other.

For normal out-of-band brightness, two successive evaluated observations must
be on the same side of the 65–85 default deadband. The factor is
`sqrt(target / median)`, clamped to 0.5–1.5. Direction reversal or entry into
the deadband clears confirmation. If either camera exceeds 0.5% clipped
samples, the controller immediately applies a 0.25–0.5 factor.

The new exposure value is quantized to 50 us and 0.05× gain, ignores changes
below 2%, uses gain 1× while raising shutter to its effective ceiling, then
raises matched analogue gain up to 3×. A configured settle holdoff follows
each change. Red/blue colour gains remain fixed white-balance controls and are
not part of brightness optimization.

## Flicker finding

Earlier aligned 60-frame traces already locate the reported flicker at or
before original ISP Y: left/right luma changed together while requested and
reported exposure/gains were fixed; ISP-to-rectified and encoded-to-PC output
were effectively identical. Rectification, matching, depth conversion,
encoding, gateway transport, and browser presentation are therefore excluded.

This leaves three credible source classes: illumination modulation, physical
XTR pulse variation, or sensor/ISP behavior. The trigger is currently a Linux
user-space GPIO low, `sleep_for(exposure)`, and GPIO high sequence. New runtime
min/max pulse telemetry can reveal large process scheduling excursions, but it
cannot prove the electrical waveform. A release conclusion still requires a
scope or logic analyzer on XTR under verified DC illumination, ideally with
raw Bayer and ISP Y captured together. Software anti-flicker quantization was
not added: the 60 Hz mains intensity period is about 8,333 us, already above
the product's 8,000 us hard ceiling, and it would not fix an unstable pulse.

## Validation matrix

The focused native test covers schema round-trip/rejection, dark convergence,
shutter-before-gain, deadband, one-sided clipping, stereo asymmetry telemetry,
direction-reversal damping, settle holdoff, dynamic period limits, limit
reporting, mode-transition continuity, invalid-update retention, and persisted
manual reload. The static UI test checks all new controls and confirmation
copy. Existing CLI, agent, gateway, SDK, capture, pipeline, and product tests
continue to run unchanged.

Desktop WSL result: 42/43 tests passed. The only failure is unrelated and
pre-existing: the local OpenCV 4.6 Python binding exposes `CharucoBoard.draw`
instead of the test's newer `generateImage` API.

## Pi 5 build, deployment, and live result

The complete production package cohort was rebuilt from this source on the
Pi 5. Pi-native CTest passed 44/44, including the real libcamera/GPIO build,
the focused exposure controller test, gateway/system-agent/API integration,
and the calibration tests. The runtime packages and web console were then
installed without replacing the unit's configuration. Before/after SHA-256
values were identical for `daemon.yaml`, `exposure.json`, `stereo.json`, and
`encoding.json`; the native daemon, gateway, and system-agent socket all
returned active after restart. The final check measured 64.2 C and
`throttled=0x0`.

A bounded live validator changed the installed 8,000 us / 3.0x Manual command
to Auto, sampled 60 unique published frames, and restored the exact original
document in a `finally` path. The first Auto command remained 8,000 us / 3.0x,
so the mode transition was continuous. The controller made no changes because
the very dark scene was already at the configured upper limit. Across the run:

- the command stayed at 8,000 us / 3.0x and both cameras reported 7,999 us /
  2.98538x, with zero left/right exposure or analogue-gain difference;
- pooled median was 2-3, maximum left/right median difference was 2, and the
  maximum clipped fraction was 0.0111%;
- 189 trigger attempts occurred with zero trigger errors and zero cadence
  deadline overruns; the maximum sensor timestamp pair delta was 40 us;
- published-frame span was 60 while capture-sequence span was 190, consistent
  with 10 Hz capture feeding the roughly 311 ms full-resolution CPU matcher;
- temperature rose from 64.8 C to 65.3 C and throttling remained `0x0`.

The same run measured the process-side low/high trigger path at 8,057-8,702 us
for an 8,000 us request, with one sample exceeding the 250 us guard. Continued
service telemetry subsequently recorded 16 such guard exceedances and a
10,256 us maximum pulse. These are not full-period deadline misses at the
current 100,000 us cadence, but they demonstrate material scheduling variation
in the user-space GPIO/sleep path. Together with the older aligned traces that
place brightness variation at or before ISP Y, this makes trigger-release
jitter the leading software-side flicker hypothesis. It does **not** prove the
electrical XTR waveform: GPIO edge latency, modulated illumination, and
sensor/ISP behavior still require the scope/DC-light/raw-Bayer test below.

The installed configuration finished in Manual at the original 8,000 us /
3.0x values. Configuration errors, trigger errors, frame-completion errors,
and request-requeue failures remained zero. Calibration authority is still
provisional and is a separate product-release gate.

## Remaining release gates

- Measure physical XTR low width for at least 60 aligned frames under verified
  DC lighting; compare electrical timing with original ISP Y and raw Bayer.
- Exercise Manual/Auto changes during a real indoor-to-outdoor drive and tune
  cadence only from image/depth evidence.
- Confirm final per-unit calibration and metric depth separately; exposure
  work does not change the current provisional calibration authority.
- Consider a hardware-timed trigger only if physical measurements show the
  user-space pulse is materially unstable.
