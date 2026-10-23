# Shared stereo auto exposure

This page retains the September 2026 controller design and its original hardware
measurements. The development controller now uses filtered, bounded adjustments,
a held scene baseline and acknowledgement from both cameras. For current
behavior, see [exposure controls](user-guide.md#exposure-and-flicker) and the
[configuration contract](config-schema.md); the two-observation confirmation and
immediate clipping reduction below describe the earlier implementation.

## Historical native controller (2026-09-16)

`depthd` now owns one exposure controller for the pair. Libcamera AE and AWB
are disabled. Manual and auto modes both emit one command containing exposure
time, analogue gain, and fixed red/blue colour gains; the command is applied to
both cameras. On this externally triggered IMX296 rig, exposure time also sets
the active-low GPIO17 XTR pulse duration. The effective shutter ceiling is the
smaller of 8,000 us and `trigger period - 250 us`, enforced by both the
controller and capture backend. The 250 us margin is an engineering guard, not
a measurement of the IMX296 electrical re-arm requirement.

The installed closed schema is `/etc/nuvo-depth/exposure.json`. Invalid or
partially written data is rejected and the last valid command remains active.
The web console and Python SDK read and update the same authority through the
root-owned system-agent allowlist. Status exposes the requested command, the
exposure, analogue gain, and digital gain returned by each camera, per-camera
metering/clipping, controller state, settle/confirmation counters, dynamic
shutter ceiling, and process-side trigger-duration extrema.

Manual mode is the shipping default. Auto mode meters the central 60% of both
original ISP luma planes, before rectification or stereo matching, with a
four-pixel sampling stride. It prefers shutter before gain, quantizes changes
to 50 µs and 0.05×, and observes the configured update/settle intervals. A
normal bright or dark correction requires two consecutive observations in the
same direction; clipping above 0.5% in either camera still reduces exposure
immediately. Entering auto starts at the live manual command, and changing auto
settings preserves the live command within the new limits, avoiding a reset to
stale seed values.
Auto corrects slow scene illumination changes; it cannot remove illumination or
trigger flicker already present in the source luma.

## Research basis and deliberately excluded complexity

The implementation follows the hardware authority first. Raspberry Pi states
that IMX296 exposure is the XTR low width plus about 14.26 us, pulse frequency
sets frame rate, and fixed shutter/gain/AWB controls are needed to prevent
camera algorithms from dropping or changing frames. Libcamera's control model
also recommends starting auto from the last manual value and preserving the
last computed value across mode transitions to avoid visible flicker:

- [Raspberry Pi Global Shutter external trigger documentation](https://www.raspberrypi.com/documentation/accessories/camera.html#external-trigger-on-the-global-shutter-camera)
- [libcamera exposure, gain, transition, and flicker controls](https://docs.libcamera.org/master/internal-api/namespacelibcamera_1_1controls.html)

Robot-vision research supports optimizing feature quality, stereo-camera
brightness consistency, and noise rather than only an image-wide average:

- [Shim et al., Gradient-based Camera Exposure Control for Outdoor Mobile Platforms](https://arxiv.org/abs/1708.07338)
- [Shin et al., Camera Exposure Control for Robust Robot Vision with Noise-Aware Image Quality Assessment](https://arxiv.org/abs/1907.12646)

Those papers justify the shared stereo command and shutter-before-gain policy,
but not adding a gradient/entropy optimizer without rig-specific drive data.
The production controller therefore stays deterministic and inexpensive. It
adds robust median/clipping diagnostics and temporal confirmation while leaving
more complex scene scoring as a future evidence-driven change.

The historical measurements below came from the earlier diagnostic controller
and explain the tuning. The native controller and live reported metadata are
the current implementation authority.

## Historical tuning contract

The native CPU pipeline uses one pair-level auto-exposure controller. It never
runs independent left/right AE loops.

Hard operating limits:

- physical shutter is the common IMX296 GPIO17 XTR low pulse;
- shutter is always between 250 and 8000 us;
- analogue gain is always between 1.0x and 3.0x;
- both cameras receive the same gain request and fixed color gains;
- 8000 us is an absolute 1/125 s ceiling, including while stationary.

The historical diagnostic controller metered a 364 x 272 rectified grayscale
pair. The current native controller instead meters the middle 60% of each
original ISP luma plane before rectification, sampling every fourth pixel.
This keeps the work small and prevents stereo/rectification output from feeding
back into camera brightness control.

## Control law

Defaults:

- target median: 75;
- accepted deadband: 65 through 85;
- dark threshold: 16;
- clipping threshold: 245;
- clipping response threshold: 0.5% in either camera;
- normal evaluation cadence: every two frames;
- settling holdoff after a change: four frames.

Outside the deadband, two consecutive observations in the same direction are
required before the requested exposure value changes proportionally to the
square root of `target / measured`. A direction reversal or an observation
inside the deadband resets confirmation. In clipping, exposure is reduced
immediately by at least half and can fall to one quarter in one update. In
darkness, one update can increase exposure by no more than 1.5x.

Exposure value is divided into shutter and gain by preferring the lowest gain:

1. use gain 1.0x and increase shutter up to the effective guarded ceiling;
2. hold shutter at that ceiling and increase matched gain only if more exposure is
   required;
3. clamp at 8000 us / 3.0x and report the remaining dark condition.

The four-frame holdoff is required because camera control requests take time to
appear in completed buffers. Without it, a fast feedback loop reacts repeatedly
to pre-change frames and overshoots.

## Hardware validation

### Dark-start convergence

A 120-frame run started deliberately at 3000 us / 1.5x. The controller reached
8000 us / 2.2x in five adjustments and finished at median 75. It did not
oscillate after the settling holdoff was introduced.

- rate: 9.948 Hz including first-frame startup;
- AE p50 / p95: 0.007 / 0.314 ms;
- sensor timestamp delta p50 / max: 18 / 20 us;
- maximum applied left/right gain difference: 0.0;
- rejected or replaced ready pairs: 0.

### Production-default 60-second soak

A 600-frame measured run started at 8000 us / 2.0x. The scene remained inside
the deadband, so no adjustment was necessary.

- rate: 9.9999 Hz;
- final ROI median: 69;
- dark fraction: 7.61%;
- clipped fraction: 0.10%;
- AE p50 / p95: 0.138 / 0.294 ms;
- normalized process CPU: 9.84%;
- depth-ready p50: 53.99 ms;
- sensor timestamp delta p50 / max: 17 / 22 us;
- maximum applied left/right gain difference: 0.0;
- paired frames: 610, with one startup timestamp rejection and no ready-pair
  replacement.

The earlier equivalent fixed-exposure run used 9.77% normalized process CPU,
so the measured AE overhead is approximately 0.06 percentage points and within
normal run-to-run system variation.

## Validation boundary

The controller tests cover convergence, one-sided clipping, left/right
asymmetry telemetry, deadband behavior, direction-reversal damping,
quantization, settling, guarded limits, mode continuity, invalid configuration,
and persistence. A physical indoor-to-outdoor drive remains necessary to tune
transition speed under real sunlight. Process-side trigger durations are useful
diagnostics but do not replace an oscilloscope or logic analyzer at XTR.

Reports and images remain outside Git on the Pi under
`/home/cam/ae-convergence-settled-20260712/` and
`/home/cam/cpu-sgbm-auto-exposure-60s-20260712/`.

### September 16 native deployment

The complete Pi production build passed 44/44 tests. A bounded 60-published-
frame Manual-to-Auto-to-Manual run preserved the live 8,000 us / 3.0x command,
kept both camera metadata matched, and restored the original configuration
byte-for-byte. Process-side XTR timing ranged from 8,057 to 8,702 us in that
run; one pulse exceeded the 250 us guard. Later in the same service lifetime,
the counter reached 16 and the maximum reached 10,256 us. There were no trigger
errors, cadence deadline overruns, or thermal throttling. See
`stereo-exposure-evaluation-20260916.md` for the complete interpretation and
release gates.
