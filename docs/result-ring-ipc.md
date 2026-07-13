# Result ring IPC

Depth Daemon and the ROS container exchange completed products through one
file-backed mmap ring at `/run/depthd/results.ring`. The ABI authority is
`include/depthd/ipc/result_ring_layout.hpp`. The ROS package must vendor the
same file; it must not recreate the structure from prose.

## Behavior

- Four fixed-capacity slots bound memory and allow the producer to continue if
  ROS pauses briefly.
- The newest complete sequence wins. The reader never queues older frames.
- Every slot uses two acquire/release sequence guards. Odd guards are being
  written; equal, stable, even guards are complete.
- Writer restarts receive a new `instance_id`. Readers remap replaced files and
  report restart, schema, torn-read, stale, and missed-sequence states.
- Payload regions are fixed offsets, so no pointers or platform-sized fields
  cross the process boundary.
- The current file size is 11,620,352 bytes. This includes four slots with
  capacity for 728x544 float disparity, optional 728x544 mono, a 512 KiB JPEG,
  and 32,768 XYZ points.

## Products

Each 10 Hz snapshot always contains:

- 728x544 float32 disparity with NaN invalid pixels;
- logical left/right rectified camera models;
- compact `base_link` obstacle centroids, including an empty clear cloud;
- capture and ready wall-clock timestamps, health, sequence, calibration id,
  valid window, and cloud diagnostics.

The rectified-left JPEG flag is present only on its 2 Hz schedule. Rectified
left mono is optional and can be enabled for semantic perception. Metric depth
and dense XYZ are deliberately absent.

The mmap transport adds copies only at the final process boundary. Camera
buffers, rectification outputs, working disparity, and the compact-cloud
accumulator remain outside the ring.

`depthd::NativeOutputPublisher` is the single downstream integration point. It
consumes the already-finished disparity, calculates the compact cloud, and
publishes the ring snapshot. `libdepthd_native_output.so` exposes the same path
through a small C ABI so the validated Picamera2 loop can be used for initial
robot integration without moving or retuning that loop.
