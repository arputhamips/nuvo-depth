# External-trigger and async zero-copy results

## Hardware correction

The supplied Pi uses two InnoMaker IMX296 color global-shutter cameras whose
TRIG inputs share BCM GPIO17 (physical header pin 11). The previous ROS setup
pulsed that line but left the upstream driver at `trigger_mode=0`, so both
sensors were free-running. Its 50 ms pairing tolerance accepted the resulting
roughly 40 ms offset as a nominal pair.

The corrected configuration is:

- `camera_auto_detect=0`;
- `dtoverlay=imx296,always-on,cam0` and
  `dtoverlay=imx296,always-on,cam1`;
- `options imx296 trigger_mode=1` in `/etc/modprobe.d`;
- GPIO17 idle high with a 3 ms low pulse at 10 Hz;
- nominal 3000 us exposure and analogue gain 1.5;
- sensor-timestamp pairing tolerance 500 us.

This follows the InnoMaker V2.0.2 manual and Raspberry Pi's IMX296 external
trigger contract. The low pulse controls exposure. The `always-on` overlays
prevent powered-down sensors from clamping their shared trigger input.

## One-pulse proof

With both streams running in external-trigger mode:

1. A 400 ms no-pulse interval produced zero completed frames on both cameras.
2. Five 10 Hz priming pulses produced one application-visible frame per camera;
   libcamera consumed the first four as startup/settling frames.
3. One additional GPIO17 pulse produced exactly one new frame per camera.
4. The two test frames' `SensorTimestamp` values differed by 14 us.

This distinguishes sensor timing from host callback timing. Picamera callbacks
arrive serially about 1.27 ms apart, but the measured sensor timestamps are
19 us apart at p50 and 22 us apart at worst in the 300-frame run. Stereo pairing
therefore uses `SensorTimestamp`, never callback arrival time.

## Async leased-buffer path

`async_stereo_capture.py` uses Picamera2 completion callbacks only to acquire a
request lease and submit it to a bounded timestamp-pairing slot. It retains at
most one pending frame per side and one ready pair. Replaced or rejected leases
are released immediately, so camera buffers cannot accumulate in an unbounded
queue.

The processing thread maps both leased requests through Picamera2 `MappedArray`
with `write=False`. This returns NumPy/OpenCV views over the camera allocator's
mapped buffers without calling `make_array()` or copying pixels. Rectification
creates owned destination images, after which both camera requests are released
before grayscale conversion and SGBM.

The full 728 x 544 -> 364 x 272 -> 728 x 544 CPU path was run twice
under the same external-trigger, 3 ms exposure, gain, SGBM, and output settings:

| Metric | Copied external-trigger path | Async zero-copy path |
| --- | ---: | ---: |
| Output rate | 10.0004 Hz | 10.0003 Hz |
| Camera input copies | 2 per pair | 0 |
| Input access p50 | 0.706 + 0.803 ms copies | 0.135 ms map |
| Process CPU, normalized over four cores | 12.49% | 12.03% |
| Whole-SoC CPU | 12.48% | 11.42% |
| Sensor timestamp delta p50 | 0.018 ms | 0.018 ms |
| Depth-ready left/right age p50 | 66.06 / 66.07 ms | 64.85 / 64.87 ms |
| Rejected/replaced measured pairs | not bounded | 0 / 0 |

Two array copies and the synchronous capture handoff were replaced by a 0.135 ms
mapping operation and callback lease handoff. Normalized process CPU fell by
0.46 percentage points, with clean request recycling and no exported-DMABUF
shutdown warnings. The whole-SoC sample is noisier than process CPU and should
not be interpreted more precisely than the displayed values.

The 30-second lease soak processed 300 measured pairs plus 10 warm-up pairs.
All 310 callbacks on each side formed pairs; no callback, pending frame, or
ready pair was rejected or replaced. Peak process RSS was 218608 KiB.

For historical context, the former free-running path had 43.59 ms median sensor
skew and 107.64 / 64.05 ms left/right depth-ready age. Enabling the actual
hardware trigger, rather than asynchronous capture itself, removed that skew.

The next independent optimization remains grayscale-before-rectification. It
does not alter this lease/pairing design.

## References

- InnoMaker camera repository and V2.0.2 manual:
  <https://github.com/INNO-MAKER/cam-imx296raw-trigger>
- Raspberry Pi IMX296 external-trigger documentation:
  <https://www.raspberrypi.com/documentation/accessories/camera.html#external-trigger-on-the-global-shutter-camera>
- Raspberry Pi IMX296 driver (`trigger_mode` implementation):
  <https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/media/i2c/imx296.c>
