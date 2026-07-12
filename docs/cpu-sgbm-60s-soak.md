# CPU SGBM 60-second robot soak

## Scope

The stable CPU backend was exercised on the supplied Raspberry Pi 5 for 600
measured stereo pairs at 10 Hz after 10 warm-up pairs. The run used the
asynchronous leased-buffer capture path, zero camera-input copies, the corrected
logical camera/calibration mapping, hardware external trigger, and the current
production-intent outputs:

- 728 x 544 rectified inputs;
- 364 x 272 SGBM working images;
- 96 working disparities, equivalent to 192 full-resolution pixels;
- OpenCV `SGBM_3WAY` with one OpenCV thread;
- float32 disparity and temporary metric-depth buffers at 10 Hz;
- quality-75 rectified-left JPEG encoding at 2 Hz;
- GPIO17 idle high with a 3 ms low pulse and 500 us pair tolerance.

The ROS container and other camera/Hailo consumers were not running. Before the
test, both C++ tests passed, IMX296 `trigger_mode` was `1`, the Pi reported no
throttling, and its temperature was 51.6 C.

## Result

The measured interval was 59.9999999 seconds and completed without a dropped,
rejected, replaced, or stale stereo pair.

| Metric | Result |
| --- | ---: |
| Measured pairs | 600 |
| Achieved output rate | 10.0000 Hz |
| Process CPU, one core = 100% | 48.14% |
| Process CPU, normalized over four cores | 12.03% |
| Whole-SoC CPU | 11.94% |
| Peak RSS | 218,880 KiB |
| Valid depth fraction in the final frame | 59.76% |
| Sensor timestamp delta, p50 / p95 / max | 17 / 19 / 22 us |
| Callback delta, p50 / p95 | 1.255 / 1.291 ms |
| Depth-ready frame age, p50 / p95 | 64.8 / 67.6 ms |
| Cycle time, p50 / p95 / max | 100.01 / 102.78 / 118.97 ms |
| End temperature | 51.6 C |
| Thermal/throttle flags | `0x0` |

The callback delta is host-side serial completion latency, not exposure skew.
Pairing used the sensor timestamps and all 610 warm-up plus measured callbacks
from each camera formed valid pairs.

## Stage profile

Pair waiting is mostly sleeping until the next 100 ms trigger and is not CPU
work. The recurring active path is approximately 44.8 ms per frame when the
2 Hz JPEG cost is amortized over the 10 Hz depth stream.

| Active stage | p50 (ms) | Approximate active-path share |
| --- | ---: | ---: |
| SGBM match | 25.661 | 57.2% |
| Rectify two full-color frames | 13.538 | 30.2% |
| Restore, resize, and mask disparity | 2.615 | 5.8% |
| Convert two images to grayscale | 0.899 | 2.0% |
| Derive metric depth | 0.884 | 2.0% |
| Copy disparity and depth outputs | 0.543 | 1.2% |
| JPEG encode at 2 Hz, amortized | 0.443 | 1.0% |
| Map both leased camera buffers | 0.138 | 0.3% |
| Downscale two grayscale images | 0.124 | 0.3% |

The two dominant stages account for about 87% of active processing. SGBM is
already using the fastest previously tested OpenCV mode. The installed OpenCV
4.10 build has AArch64 NEON/FP16 baseline code and NEON dot-product dispatch,
so rebuilding merely to enable SIMD is not an optimization opportunity.

## Implemented grayscale-first result

The depth preprocessing was changed without changing the camera format. Both
cameras remain `RGB888` (BGR byte order in Picamera/OpenCV memory). Every frame
is converted to grayscale before remapping. The full-color left image is
remapped only for the 2 Hz JPEG output.

A second 600-frame run verified the change:

| Metric | Color-first baseline | Grayscale-first | Change |
| --- | ---: | ---: | ---: |
| Output rate | 10.0000 Hz | 10.0000 Hz | unchanged |
| Process CPU, one core = 100% | 48.14% | 41.96% | -12.8% |
| Process CPU, normalized over four cores | 12.03% | 10.49% | -1.55 points |
| Whole-SoC CPU | 11.94% | 10.96% | -0.98 points |
| Recurring two-camera rectification | 13.538 ms | 5.355 ms | -8.183 ms |
| Grayscale conversion | 0.899 ms | 0.894 ms | unchanged |
| Throttled left-color rectification | included every frame | 6.774 ms at 2 Hz | scheduled |
| SGBM match | 25.661 ms | 25.677 ms | unchanged |
| Depth-ready age p50 | 64.8 ms | 56.6 ms | -8.2 ms |
| Sensor delta p50 / max | 17 / 22 us | 17 / 20 us | stable |
| Rejected or replaced pairs | 0 | 0 | stable |
| Peak RSS | 218,880 KiB | 215,264 KiB | -3,616 KiB |
| End temperature / throttling | 51.6 C / none | 50.5 C / none | stable |

The process used 12.8% less CPU while preserving the exact 10 Hz cadence and
bounded zero-copy capture behavior. The saved JPEG was visually checked: blue
and red remain correctly ordered. The final-frame valid fraction is not a
before/after quality metric because the physical scene changed between live
runs. The controlled recorded-pair comparison remains the quality evidence: it
measured zero median and 0.313 working-pixel p95 disparity change.

The optimized JSON, grayscale rectified pair, color JPEG, disparity/depth arrays,
and end-state log are under
`artifacts/cpu_sgbm/cpu-sgbm-gray-first-60s-20260712/` on the workstation and
`/home/cam/cpu-sgbm-gray-first-60s-20260712/` on the Pi.

## Recommended next optimization order

1. Do not derive or copy metric depth in the production loop unless a subscriber
   requires it. The required public output is disparity. Omitting metric depth
   and its output copy should save about 1.1 ms per frame.
2. In the production C++ daemon, preallocate rectified images, grayscale images,
   working images, fixed-point disparity, float disparity, validity masks, and
   publication buffers. The Python profiler allocates several of these per
   frame, contributing to its roughly 3.3 ms gap between measured stage time
   and total process CPU.
3. Keep direct 364 x 272 rectification experimental. It saves another roughly
   4 ms, but the replay comparison changed p95 disparity by 2.125 working pixels
   (4.25 output pixels), which is too large to adopt without scene/range tests.
4. Keep `SGBM_3WAY` and one OpenCV thread. The other OpenCV SGBM modes measured
   between 40 and 54 ms versus 25.7 ms for `SGBM_3WAY`. More threads may reduce
   wall latency but do not serve the primary goal of minimum aggregate CPU.
5. Evaluate a camera YUV/dual-stream mode separately. Reading an ISP-produced Y
   plane may remove the remaining 0.9 ms grayscale conversion, but its plane
   stride, buffer lifetime, color-output conversion, and synchronization must be
   validated before replacing the stable `RGB888` capture contract.
6. Measure ROS message wrapping, serialization, and publish time after the ROS
   bridge exists. Those costs were intentionally outside this test.

The full JSON report, final rectified pair, disparity/depth arrays, and camera
log remain reproducibly stored outside Git under
`artifacts/cpu_sgbm/cpu-sgbm-60s-20260712/` on the workstation and
`/home/cam/cpu-sgbm-60s-20260712/` on the Pi.
