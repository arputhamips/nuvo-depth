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
   production `isp_luma_rgb_left` profile maps each camera's full-resolution
   YUV420 Y plane directly for depth and creates a secondary RGB stream only
   on the left camera. The left RGB view is rectified only when JPEG or an
   enabled semantic model is due. `rgb_gray` retains the earlier two-camera
   RGB capture as an explicit rollback profile. Metric depth is disabled by
   default; pass `--metric-depth` only for validation runs that need a depth
   array. `--working-width`, `--working-height`, and `--min-disparity` support
   native-resolution and bounded-range experiments without changing
   production defaults.
   An optional `egl_gl_dmabuf` selection imports the leased Y planes directly
   into EGL/OpenGL without entering `MappedArray`; its native library path is
   supplied with `--egl-gl-rectification-library`. CPU mapping is lazy and
   occurs only for explicit `cpu_opencv`, `--compare-rectification-backends`,
   or a configured GPU failure fallback. See
   `docs/egl-gl-dmabuf-rectification.md`; this candidate is default off.
8. `stereo_auto_exposure.py` meters both working grayscale images as one stereo
   instrument and updates the shared XTR shutter plus matched camera gain. It
   enforces absolute 8 ms shutter and 3x gain ceilings.
9. `benchmark_stereo_bm.py` replays a rectified pair through OpenCV StereoBM
   with explicit range, block, uniqueness, speckle, consistency, and texture
   settings. It is an experiment tool and is not a production backend.
10. `validate_ros_bridge.py` subscribes to the native ROS bridge outputs and
    fails on topic-contract, calibration, rate, timestamp, compact-cloud,
    diagnostics, duplicate-publisher, or disabled-output regressions. Run it in
    a ROS Jazzy environment while the native producer and `depthd_ros_bridge`
    are active.

The async profiler can also exercise the additive native output boundary while
leaving its default path unchanged. Build `libdepthd_native_output.so` and add:

```bash
--result-ring /run/depthd/results.ring \
--native-output-library ./build/libdepthd_native_output.so
```

Add `--publish-left-mono-to-ring` only for an explicit grayscale diagnostic
consumer. Native semantics branches from the owned full-resolution rectified
BGR frame before SGBM; it never consumes rectified grayscale or the 2 Hz JPEG.
SGBM continues independently on rectified grayscale. Disparity, CameraInfo
metadata, and the compact `base_link` cloud are present on every native result;
JPEG is present only on its existing 2 Hz schedule.

Native semantic perception is separately gated and remains off unless
`--semantic` is supplied. It requires both the native result ring and the two
verified HEFs:

```bash
--semantic \
--semantic-ring /run/depthd/semantic.ring \
--semantic-runtime-library ./build/libdepthd_native_semantic.so \
--semantic-yolo-hef /var/lib/nuvo-depthd/current/models/hailo/front_semantic.hef \
--semantic-segformer-hef /var/lib/nuvo-depthd/current/models/hailo/segformer_b0_bn.hef
```

Production assets are installed and verified by `depthd-assets`; see the
[deployment guide](../../deployment/README.md). ROS deployment never provisions
these files.

Build the Pi runtime against its installed HailoRT development package:

```bash
cmake -S . -B build \
  -DDEPTHD_WITH_HAILORT_SEMANTICS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Python retains only a capacity-one lease containing the existing NumPy color
and disparity owners. A short `ctypes` call passes their pointers to C++; it
does not copy either full-resolution buffer. Replaced and completed leases are
released exactly once. Resize and BGR-to-RGB conversion happen only in
model-sized native staging buffers. One persistent Hailo `VDevice`, two
persistent configured `InferModel` bindings, native post-processing, same-frame
depth fusion, and `semantic.ring` publication all live behind that seam. The
worker serializes the two models on the single Hailo-8L, and raw masks never
enter mmap.

`--semantic-preview` optionally adds a diagnostic JPEG to `semantic.ring`; it
is disabled by default and does not change the normal 2 Hz ROS JPEG product.

The legacy ROS camera/depth stack must be stopped before the native profiler so
the cameras have one owner. The small read-only ROS bridge and its validator do
not own camera or Hailo devices and are intended to run alongside the native
producer.
