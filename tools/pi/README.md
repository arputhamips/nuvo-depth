# Temporary Pi validation tools

These Python tools validate the first HEF against the actual camera rig. They
are deliberately outside the production frame loop.

1. `capture_rectified_pair.py` configures both IMX296 cameras at 728 x 544
   RGB888 with 180-degree orientation, enables `SyncMode=2`, pulses GPIO 17 at
   10 Hz, captures one pair, and applies the Nuvo stereo calibration.
2. `validate_hef.py` replicate-pads the rectified images to 736 x 544, runs the
   two-input HEF, crops back to 728 x 544, derives metric depth for diagnostics,
   and reports device-only latency.

The ROS container must be stopped before using either tool so cameras and the
Hailo device each have a single owner.
