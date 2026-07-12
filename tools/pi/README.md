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
   10 Hz, captures one pair, and applies the Nuvo stereo calibration.
2. `validate_hef.py` adapts the rectified pair to the HEF input geometry, runs
   either a one-output or split two-output model, restores 728 x 544 calibrated
   disparity, derives metric depth, and reports device and host-postprocess
   latency. With `--reference-disparity`, it also emits a difference image and
   pixel-error percentiles against a known ONNX output.

The ROS container must be stopped before using either tool so cameras and the
Hailo device each have a single owner.
