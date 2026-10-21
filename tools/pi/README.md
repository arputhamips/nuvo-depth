# Raspberry Pi depth-camera tools

For device installation and first use, follow the
[Pi installation guide](https://docs.nuvohardware.com/vision-one/install/byo-pi)
and [setup guide](https://docs.nuvohardware.com/vision-one/guide/getting-started).
The native `depthd` daemon is the camera owner. The former Python processing
pipeline and its private result-ring/obstacle-cloud libraries have been retired.
Use SDK/gateway observations for live processing measurements.

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
4. `profile_production_resources.py` samples the native services and optional
   encoded stream while they run.
5. `collect_live_performance.py` collects matcher counters and timings through
   the gateway relay without changing device controls.
6. `benchmark_preprocess_variants.py`, `benchmark_stereo_bm.py`, and related
   replay tools compare alternatives using saved input pairs.
7. `stereo_auto_exposure.py` provides shared stereo metering for rig diagnostics.
8. `validate_ros_bridge.py` validates a separately supplied ROS bridge. The
   native appliance does not publish ROS topics or a private obstacle-cloud ring.

Do not run direct camera tools alongside the native capture service. The native
CPU and optional EGL/DMABUF rectification backends remain available; their
configuration and fallback behavior are described in the runtime documentation.
Keep a depth subscription active when using `collect_live_performance.py`:
health polling alone does not request matching. Its observations are sampled
matcher results, rather than received depth packets.

The package profile and calibration seed are documented in
[`docs/pi5-depth-100mm-profile.md`](../../docs/pi5-depth-100mm-profile.md).
Production installation and asset verification are documented in the
[`deployment` guide](../../deployment/README.md).

