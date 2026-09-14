# Raspberry Pi depth-camera tools

These tools validate and profile the Nuvo 100 mm dual-IMX296 depth camera on
Raspberry Pi 5. The product pipeline is depth-only: synchronized capture,
rectification, stereo matching, preview, and native depth outputs. It has no
semantic-inference or accelerator dependency.

## Camera setup and validation

`focus_stream.py` serves a full-sensor MJPEG stream from the logical left
camera. It defaults to camera index 1, 1456 x 1088 at 10 Hz, applies the product
180-degree orientation, and listens on port 8000.

```bash
python3 tools/pi/focus_stream.py --camera-index 1 --port 8000
```

Open `http://PI_ADDRESS:8000/` from a machine on the same network. Stop it with
Ctrl+C before starting the depth daemon so that the cameras have one owner.

The other rig tools are:

1. `capture_rectified_pair.py` configures both IMX296 cameras at 728 x 544,
   enables `SyncMode=2`, pulses GPIO 17 at 10 Hz, captures one pair, and writes
   raw and rectified images for camera-order and calibration checks.
2. `verify_external_trigger.py` verifies that one primed trigger pulse produces
   one frame per camera and reports their sensor timestamp delta.
3. `audit_imx296_trigger.sh` reports the driver trigger parameter, boot
   overlays, detected cameras, and GPIO 17 state.
4. `profile_cpu_pipeline.py` measures the live OpenCV SGBM path, including
   cold-start, capture, per-stage, sensor-age, output-latency, and CPU data.
5. `profile_async_zero_copy_pipeline.py` is the product runtime entrypoint. It
   combines leased Picamera2 completions, bounded latest-pair matching, GPU or
   CPU rectification, CPU SGBM, scheduled JPEG preview, and native outputs.
6. `benchmark_preprocess_variants.py`, `benchmark_stereo_bm.py`, and related
   replay tools compare depth-processing alternatives without changing the
   product profile.
7. `stereo_auto_exposure.py` meters the stereo pair as one instrument and
   applies a shared shutter and matched gain.
8. `validate_ros_bridge.py` checks the optional read-only ROS bridge contract
   while the native producer is running.

The product capture profile uses full-resolution YUV420 luma for depth and a
left-camera color stream for preview. `--preview-source raw` encodes the raw
left view; `--preview-source rectified` aligns the preview with the depth
result. The `egl_gl_dmabuf` rectifier is preferred, with `cpu_opencv` available
as the configured fallback.

Metric depth arrays remain optional in the Python layer because the native
output library derives the compact obstacle cloud from disparity and calibrated
camera metadata. To exercise the native output boundary manually, build
`libdepthd_native_output.so` and add:

```bash
--result-ring /run/depthd/results.ring \
--native-output-library ./build/libdepthd_native_output.so
```

Disparity, CameraInfo metadata, diagnostics, and the compact `base_link` cloud
are written for every result. JPEG is written on its configured lower-rate
schedule. Add `--publish-left-mono-to-ring` only for an explicit grayscale
diagnostic consumer.

The package profile and calibration seed are documented in
[`docs/pi5-depth-100mm-profile.md`](../../docs/pi5-depth-100mm-profile.md).
Production installation and asset verification are documented in the
[`deployment` guide](../../deployment/README.md).

`validate_hef.py` is retained only as an uninstalled historical experiment; it
is not called by the product runtime, service, tests, or Debian package.
