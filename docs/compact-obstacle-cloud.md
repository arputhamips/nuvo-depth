# Native compact obstacle cloud

## Purpose

`depthd::CompactObstacleCloudGenerator` converts the locked 728 x 544 float32
disparity product directly into the compact obstacle representation used by
Nuvo navigation. It is ROS-free and does not construct a dense point cloud.

The module is downstream of stereo matching. It cannot change capture,
auto-exposure, calibration mapping, rectification, or SGBM behavior. Those
defaults and the cloud defaults are enforced by
`depthd_production_profile_lock_test`.

## Processing

For each frame the generator:

1. intersects the configured row ROI with the disparity valid window;
2. samples disparity with the configured row and column strides;
3. rejects nonfinite, bounded-disparity, and out-of-range depth samples;
4. reconstructs optical XYZ from the calibrated camera model;
5. applies a caller-provided optical-to-target rigid transform;
6. filters height, forward direction, horizontal angle, and planar range;
7. accumulates accepted samples directly into 5 cm target-frame voxels;
8. rejects voxels with insufficient raw support;
9. finds 26-neighbor voxel clusters and rejects undersupported clusters;
10. returns one centroid per accepted voxel, sorted deterministically.

No intermediate dense XYZ image is created. The voxel hash table, traversal
stack, cluster scratch space, and result storage are fixed-capacity allocations
created with the generator. Steady-state calls reuse them. Capacity exhaustion
is reported through `voxel_capacity_overflow` and is covered by regression
tests.

## Locked navigation defaults

| Setting | Value |
|---|---:|
| Disparity ROI rows | 96 through 419 |
| Pixel sampling | 2 x 2 |
| Depth / planar range | 0.75 to 6.0 m |
| Target-frame height | 0.18 to 1.20 m |
| Horizontal angle | -0.70 to 0.70 rad |
| Voxel leaf | 0.05 m |
| Raw samples per voxel | 5 |
| Voxels per cluster | 12 |
| Raw samples per cluster | 60 |
| Maximum accumulated voxels | 32768 |

These reproduce the deployed `nuvo_depth_obstacles` cloud path. The production
output frame is `base_link`; the transform is an input rather than a compiled
robot-mount constant so the ROS bridge can remain the TF authority.

## Recorded-frame benchmark

Input:
`artifacts/cpu_sgbm/production-sweep-20260712/geometry/f128_w64_b5/disparity_float32.bin`

| Metric | Pi 5 |
|---|---:|
| Compute p50 | 1.414 ms |
| Compute p95 | 1.420 ms |
| Compute max | 1.494 ms |
| Sampled pixels | 48,600 |
| Accepted raw points | 7,588 |
| Accumulated / supported voxels | 2,251 / 384 |
| Accepted clusters | 5 |
| Output centroids | 270 |
| Capacity overflow | 0 |

The standalone benchmark is `depthd_obstacle_cloud_benchmark`. Its optional
CSV output is intended for ROS-projector parity checks and visualization, not
as a runtime transport.
