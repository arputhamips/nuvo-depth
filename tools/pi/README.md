# Temporary Pi validation tools

These Python tools validate the first HEF against the actual camera rig. They
are deliberately outside the production frame loop.

## Manual focus stream

`focus_stream.py` serves a full-sensor, browser-viewable MJPEG stream from the
logical left camera. It defaults to camera index 1, 1456 x 1088 at 10 Hz,
applies the production 180-degree orientation, and listens on port 8000.

```bash
python3 tools/pi/focus_stream.py --camera-index 1 --port 8000
```

Open `http://PI_ADDRESS:8000/` from a machine on the same network. Stop it with
Ctrl+C; the camera must be released before starting capture or the depth daemon.

1. `capture_rectified_pair.py` configures both IMX296 cameras at 728 x 544
   RGB888 with 180-degree orientation, enables `SyncMode=2`, pulses GPIO 17 at
   10 Hz, captures one pair, and applies the Nuvo stereo calibration. It saves
   both oriented raw frames and rectified frames so camera/calibration mapping
   errors can be diagnosed independently. `--trigger-low-ms`,
   `--analogue-gain`, and `--colour-gains RED BLUE` support repeatable exposure
   sweeps while keeping the two cameras photometrically locked.
2. `validate_hef.py` adapts the rectified pair to the HEF input geometry, runs
   either a one-output or split two-output model, restores 728 x 544 calibrated
   disparity, derives metric depth, and reports device and host-postprocess
   latency. With `--reference-disparity`, it also emits a difference image and
   pixel-error percentiles against a known ONNX output.
3. `profile_cpu_pipeline.py` runs the live CPU SGBM path and records cold-start,
   capture, per-stage, sensor-age, output-latency, and CPU measurements. Use
   `--no-capture-flush` to model the bounded continuous production path.
4. `benchmark_preprocess_variants.py` replays one raw pair to compare color-first,
   grayscale-first, and direct-working-resolution rectification, including
   disparity differences and all OpenCV SGBM execution modes.
5. `audit_imx296_trigger.sh` reports the loaded driver trigger parameter, boot
   overlays, cameras, and GPIO17 state.
6. `verify_external_trigger.py` proves that a primed single low pulse produces
   one frame per camera and reports their sensor timestamp delta.
7. `async_stereo_capture.py` implements leased Picamera2 completions, bounded
   latest-pair matching, and zero-copy `MappedArray` access. The corresponding
   full pipeline measurement is `profile_async_zero_copy_pipeline.py`. The
   profiler converts the mapped BGR views to grayscale before depth
   rectification and rectifies the full-color left view only on the throttled
   JPEG schedule. Metric depth is disabled by default; pass `--metric-depth`
   only for validation runs that need a depth array. `--working-width`,
   `--working-height`, and `--min-disparity` support native-resolution and
   bounded-range experiments without changing production defaults.
8. `stereo_auto_exposure.py` meters both working grayscale images as one stereo
   instrument and updates the shared XTR shutter plus matched camera gain. It
   enforces absolute 8 ms shutter and 3x gain ceilings.
9. `benchmark_stereo_bm.py` replays a rectified pair through OpenCV StereoBM
   with explicit range, block, uniqueness, speckle, consistency, and texture
   settings. It is an experiment tool and is not a production backend.

The async profiler can also exercise the additive native output boundary while
leaving its default path unchanged. Build `libdepthd_native_output.so` and add:

```bash
--result-ring /run/depthd/results.ring \
--native-output-library ./build/libdepthd_native_output.so
```

Add `--publish-left-mono-to-ring` only when semantic perception needs the
already-computed rectified grayscale plane. Disparity, CameraInfo metadata, and
the compact `base_link` cloud are present on every native result; JPEG is
present only on its existing 2 Hz schedule.

The ROS container must be stopped before using either tool so cameras and the
Hailo device each have a single owner.
