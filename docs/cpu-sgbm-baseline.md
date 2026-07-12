# CPU SGBM baseline

Measured on 2026-07-12 on the supplied Raspberry Pi 5 using OpenCV 4.10.0.
The ROS container was stopped. The benchmark replayed one rectified 728 x 544
pair so the numbers isolate the stereo backend; live capture, trigger handling,
and rectification are not included.

## Backend configuration

- logical input/output: 728 x 544;
- internal matching: 364 x 272;
- physical full-resolution disparity range: 192 pixels;
- internal disparity range: 96 pixels;
- OpenCV `MODE_SGBM_3WAY`, block size 5;
- one OpenCV worker thread;
- disparity is restored to 728 x 544 and multiplied by two;
- invalid pixels are emitted as IEEE float32 NaN.

Half-resolution 96-disparity matching covers the same physical range as
full-resolution 192-disparity matching. It preserves the output contract but
does not preserve all full-resolution spatial detail.

## Sustained replay results

| Metric | 10 Hz, 600 frames | 6 Hz, 360 frames |
| --- | ---: | ---: |
| Achieved rate | 10.000 Hz | 6.000 Hz |
| Deadline misses | 0 | 0 |
| Compute p50 | 37.104 ms | 39.330 ms |
| Compute p95 | 38.065 ms | 44.471 ms |
| Compute p99 | 39.287 ms | 44.517 ms |
| Preprocess p50 | 1.313 ms | 1.359 ms |
| SGBM match p50 | 33.914 ms | 35.889 ms |
| Postprocess p50 | 1.879 ms | 2.014 ms |
| Process CPU (`100%` = one core) | 36.156% | 23.395% |
| Normalized four-core CPU | 9.039% | 5.849% |
| Valid disparity fraction | 54.2% | 54.2% |

Temperature was 48.3 C before the tests, 48.8 C after the 10 Hz run, and
48.3 C after the 6 Hz run. This is a short functional baseline, not the required
30-minute thermal acceptance run.

## Camera and color findings

The supplied rig produces coherent positive disparity when libcamera camera 1
is logical left and camera 0 is logical right. The reverse ordering produced
only 42.4% valid pixels and a visibly fragmented map, versus 54.2% and a
coherent scene with the corrected order. The runtime profile and capture tool
therefore default to `left=1`, `right=0`, with both values remaining explicit
and configurable.

Picamera2 inherits libcamera's counterintuitive format naming: `RGB888` is B, G,
R byte order in memory and is already compatible with OpenCV. The prior
diagnostic capture converted it from RGB to BGR a second time, swapping red and
blue. That conversion has been removed.

The camera-order result must be reconfirmed after focus repair with a newly
captured, correctly colored, synchronized pair. The old pair remains suitable
for deterministic performance testing but not accuracy acceptance.

The later resolution, disparity, filtering, and thread-count comparisons are
recorded in [cpu-sgbm-test-matrix.md](cpu-sgbm-test-matrix.md).

## Focused-scene validation

A new well-lit multi-depth scene was captured after both lenses were focused.
Both images are sharp and correctly colored. This exposed an additional mapping
detail: the calibration files are tied to the individual camera units. With the
current physical arrangement, logical left camera 1 must use
`front_right.yaml`, while logical right camera 0 must use `front_left.yaml`.

Assigning calibration by logical side left a median rectified vertical residual
of 20.2 px. Assigning it by camera unit reduced the median to 0.34 px. SGBM must
not run against the former mapping even though it can still produce a visually
plausible colored image.

The selected focused-scene configuration is:

- 364 x 272 internal matching and 728 x 544 float32 output;
- 96 internal / 192 full-resolution disparities;
- `MODE_SGBM_3WAY`, block size 5;
- uniqueness ratio 5;
- speckle window 50 and speckle range 2;
- one OpenCV worker thread.

In a 600-frame replay at 10 Hz it achieved:

| Metric | Focused scene |
| --- | ---: |
| Achieved rate | 10.000 Hz |
| Deadline misses | 0 |
| Compute p50 / p95 / p99 | 37.326 / 37.524 / 39.180 ms |
| Process CPU (`100%` = one core) | 36.400% |
| Normalized four-core CPU | 9.100% |
| Valid fraction, complete image | 53.7% |
| Valid fraction, searchable region | 73.0% |
| Temperature before / after | 47.7 / 48.3 C |

Sample region medians were 0.67 m for the close foreground box, 1.47 m for the
ladder region, and 5.77 m for the garage-wall region. These are scene-consistent
sanity checks, not surveyed ground truth.

An unmasked left-right WLS filter was also evaluated. It creates a visually
dense map but fills uncertain occlusions. Applying its confidence mask produces
a safer but substantially sparser result and raises one-thread matching time to
about 51.5 ms. The initial backend therefore keeps the simpler tuned SGBM output
and invalidates rejected pixels instead of manufacturing dense depth.

The capture is still not motion-valid: the selected sensor timestamps were
44.04 ms apart and both cameras reported `SyncReady=false`. The static scene is
valid for rectification, depth, and CPU testing, but hardware synchronization
must be corrected before moving-platform acceptance.

## Reproduction

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure

./build/depthd_sgbm_benchmark \
  --left left_rect.png \
  --right right_rect.png \
  --iterations 600 \
  --warmup 10 \
  --target-hz 10 \
  --opencv-threads 1 \
  --output-dir results
```
