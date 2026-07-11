# Nuvo compatibility contract

This document is derived from a read-only audit of
`Documents/nuvo-base` on 2026-07-10. The source workspace was not modified.

## Existing physical and capture contract

- Raspberry Pi 5 with camera indices 0 (left) and 1 (right).
- Two IMX296 cameras on a nominal 0.160 m rig baseline; mounted at x=0.760 m,
  z=0.180 m in `base_link`.
- Frame IDs: `camera_front_left_optical` and
  `camera_front_right_optical`.
- 728 x 544 RGB888, orientation 180 degrees.
- 10 Hz / 100000 us fixed frame duration.
- Hardware trigger enabled on `/dev/gpiochip0`, BCM line 17, 1000 us active-high
  pulse, with a 4 s camera-start delay.
- Sensor timestamps are used (`use_node_time=false`).
- Manual exposure/gain are currently disabled in the production profile; depth
  validation notes suggest 6000 us and gain 1.5 as a tuning starting point.

The right projection matrix yields a calibrated baseline of
`79.6866968475 / 493.542046188 = 0.161458780 m`.

## ROS output contract

Required:

| Topic | Type | Rate/QoS intent |
| --- | --- | --- |
| `/camera/front/disparity` | `stereo_msgs/msg/DisparityImage` | 6-10 Hz, depth 1 |
| `/camera/front/left/image_rect/compressed` | `sensor_msgs/msg/CompressedImage` | 2 Hz default, JPEG |
| `/camera/front/left/camera_info` | `sensor_msgs/msg/CameraInfo` | on start/change, transient local |
| `/camera/front/depthd/diagnostics` | `diagnostic_msgs/msg/DiagnosticArray` | 1 Hz plus state changes |

The disparity nested image is 728 x 544 `32FC1`, stamped with the original pair
capture time and the left optical frame. The message carries focal length,
baseline, min/max disparity, and quantization step. Invalid pixels are NaN.

Recommended defaults are best-effort keep-last(1) for large live data and
reliable/transient-local for camera info. If an existing Nuvo consumer still
requires reliable disparity, the bridge can change QoS without touching the
daemon.

Future, disabled by default:

| Topic | Type | Notes |
| --- | --- | --- |
| `/camera/front/obstacles` | `sensor_msgs/msg/PointCloud2` | 0.05 m voxelized compact cloud |
| `/camera/front/obstacle_scan` | `sensor_msgs/msg/LaserScan` | diagnostic/fallback only |

Dense `/camera/front/points2`, raw stereo topics, and full-rate color-rectified
topics are deliberately outside the default contract.

## Behavioral compatibility

The existing pipeline pairs likely same-trigger frames within 50 ms, uses queue
depth 4-5, then normalizes stamps before exact-sync rectification. `depthd`
should instead validate each hardware-trigger pair before processing and expose:

- original left/right sensor-stamp delta;
- pair drops and per-camera drops;
- capture-to-ready latency and output age;
- rectification/inference/JPEG timing;
- Hailo temperature/runtime errors;
- calibration ID and model build ID.

No stale frame may be republished as new. On lost sync, stale input, calibration
mismatch, or accelerator fault, health changes to degraded/fault and the result
stream stops until a fresh valid pair is available.
