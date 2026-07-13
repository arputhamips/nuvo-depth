# Shared stereo auto exposure

## Contract

The native CPU pipeline uses one pair-level auto-exposure controller. It never
runs independent left/right AE loops.

Hard operating limits:

- physical shutter is the common IMX296 GPIO17 XTR low pulse;
- shutter is always between 250 and 8000 us;
- analogue gain is always between 1.0x and 3.0x;
- both cameras receive the same gain request and fixed color gains;
- 8000 us is an absolute 1/125 s ceiling, including while stationary.

The controller meters the existing 364 x 272 rectified grayscale pair. Its ROI
is x=80..363 and y=48..209, corresponding to full-resolution x=160..727 and
y=96..419. Sampling every fourth pixel keeps the control work small.

## Control law

Defaults:

- target median: 75;
- accepted deadband: 65 through 85;
- dark threshold: 16;
- clipping threshold: 245;
- clipping response threshold: 0.5% in either camera;
- normal evaluation cadence: every two frames;
- settling holdoff after a change: four frames.

Outside the deadband, the requested exposure value changes proportionally to
the square root of `target / measured`. In clipping, exposure is reduced by at
least half and can fall to one quarter in one update. In darkness, one update
can increase exposure by no more than 1.5x.

Exposure value is divided into shutter and gain by preferring the lowest gain:

1. use gain 1.0x and increase shutter up to 8000 us;
2. hold shutter at 8000 us and increase matched gain only if more exposure is
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

The controller's bright/clipping response is unit-tested, and its dark response
and camera-control synchronization are hardware-tested. A physical indoor to
outdoor drive remains necessary to tune transition speed under real sunlight.
The hard shutter and gain limits do not depend on that tuning.

Reports and images remain outside Git on the Pi under
`/home/cam/ae-convergence-settled-20260712/` and
`/home/cam/cpu-sgbm-auto-exposure-60s-20260712/`.
