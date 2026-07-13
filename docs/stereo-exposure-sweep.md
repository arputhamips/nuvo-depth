# Stereo exposure sweep

## Purpose

This experiment measures whether the fixed 3 ms external-trigger exposure is
discarding useful stereo texture, and selects an initial operating point for a
future pair-level exposure controller.

The supplied IMX296 cameras were captured at 10 Hz through their shared GPIO17
external trigger. In this mode, the trigger low-pulse controls physical shutter
time. Both cameras used the same analogue gain and fixed common red/blue color
gains of 2.4771/2.3425. Every pair was rectified and replayed through the
selected CPU backend:

- 728 x 544 rectified grayscale;
- 364 x 272 SGBM working image;
- 80 / 160 disparity range;
- block size 5, uniqueness 1, speckle 30/2;
- one OpenCV thread.

The first sweep was rejected because a person crossed in front of the rig. All
results below come from the repeated clean sweep. Sensor timestamp separation
was 15 through 18 us for all six pairs.

## Results

The autonomy ROI is x=160..727 and y=96..419. It excludes the deterministic
left disparity-search boundary and matches the vertical obstacle-processing
region.

| Trigger / gain | ROI median | ROI pixels below 16 | ROI horizontal gradient mean | ROI valid disparity | Full-frame valid |
| --- | ---: | ---: | ---: | ---: | ---: |
| 3 ms / 1.5x | 29 | 28.87% | 10.77 | 89.34% | 66.2% |
| 5 ms / 1.5x | 42 | 18.70% | 15.55 | 88.76% | 65.4% |
| 8 ms / 1.5x | 61 | 11.19% | 20.98 | 88.18% | 64.8% |
| 12 ms / 1.5x | 82 | 5.63% | 26.04 | 88.12% | 63.1% |
| 8 ms / 2.0x | 75 | 7.22% | 24.62 | 88.14% | 63.4% |
| 8 ms / 3.0x | 102 | 1.30% | 28.56 | 88.35% | 63.6% |

At 8 ms / 2.0x, ROI median intensity is 2.59 times the 3 ms baseline and
horizontal gradient energy is 2.29 times higher. ROI valid coverage changes by
only -1.20 percentage points. The mean correspondence residual, normalized by
median intensity, improves from 0.168 to 0.131. Highlight clipping remains only
0.04% of the full image.

The darker frame reports slightly more full-frame disparity, but this is not
evidence that those extra pixels are more accurate. The brighter maps expose
more object structure and reject some smooth, weakly constrained regions. No
ground-truth depth was available for this sweep.

A 100-iteration replay confirmed that all three relevant points sustain 10 Hz
without deadline misses:

| Trigger / gain | Compute p50 | Match p50 | Normalized CPU |
| --- | ---: | ---: | ---: |
| 3 ms / 1.5x | 33.53 ms | 30.08 ms | 7.70% |
| 8 ms / 2.0x | 34.69 ms | 31.17 ms | 7.84% |
| 12 ms / 1.5x | 34.08 ms | 30.61 ms | 7.80% |

## Decision

Use 8 ms / 2.0x as the initial robot exposure seed. It recovers most of the
useful shadow signal while limiting motion exposure to 8 ms. The 12 ms result
is retained only as experimental evidence; production and validation tools now
enforce an absolute 8 ms (1/125 s) ceiling even while stationary. Gain is
allowed to rise dynamically when the shutter reaches that ceiling, but is
hard-limited to 3.0x.

The future shared exposure controller should initially target:

- ROI median intensity 70 through 85;
- fewer than 10% of ROI pixels below 16;
- fewer than 1% of ROI pixels above 245;
- one common trigger pulse and matched camera gain;
- an absolute 8 ms shutter ceiling;
- fast exposure reduction on clipping and damped increases in darkness.

The complete ignored artifacts are stored locally under
`artifacts/camera/exposure-sweep-clean-20260712/` and on the Pi under
`/home/cam/exposure-sweep-clean-20260712/`.

## References

- Raspberry Pi external trigger documentation:
  <https://www.raspberrypi.com/documentation/accessories/camera.html#external-trigger-on-the-global-shutter-camera>
- Picamera2 controls manual:
  <https://datasheets.raspberrypi.com/camera/picamera2-manual.pdf>
- Shim et al., gradient-based multi-camera exposure control:
  <https://arxiv.org/abs/1708.07338>
