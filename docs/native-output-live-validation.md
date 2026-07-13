# Native output live validation

Date: 2026-07-12

The locked CPU profile was run on the Raspberry Pi 5 with the native compact
cloud and result-ring path enabled. ROS was not running, the Hailo service was
stopped during the measurement, and it was restored afterward.

The run used the existing production settings unchanged:

- logical cameras 1 / 0, 728x544 RGB888, 180-degree orientation;
- GPIO17 external trigger at 10 Hz, 8 ms maximum shutter, 3x gain ceiling;
- grayscale-first CPU rectification;
- 364x272 SGBM working geometry, 64 working / 128 restored disparity,
  5x5 block, uniqueness 1, speckle 30/2, one OpenCV thread;
- no metric-depth image;
- JPEG quality 75 at 2 Hz.

The only enabled addition was:

```text
finished float disparity
  -> native compact base_link cloud
  -> /run/depthd/results.ring
```

## Results

| Metric | Result |
|---|---:|
| Frames measured | 60 |
| Achieved rate | 9.999 Hz |
| Process CPU, one core = 100% | 38.94% |
| Normalized Pi four-core CPU | 9.73% |
| Native cloud + ring p50 / p95 | 2.036 / 2.501 ms |
| Compact cloud p50 / p95 | 1.690 / 2.102 ms |
| Ring publication p50 / p95 | 0.242 / 0.303 ms |
| Final valid disparity pixels | 274,727 / 396,032 |
| Final compact points | 313 |
| Final supported voxels / clusters | 394 / 6 |
| Voxel capacity overflow | 0 |
| Reader torn/schema errors | 0 / 0 |
| Ring file size | 11,620,352 bytes |

The final slot had sequence 66 because six warm-up frames were also published.
Its capture-to-ready latency was 53.16 ms. The result inspector recovered the
complete disparity and cloud without a schema mismatch or torn read.

The profiler still makes its existing 0.29 ms artifact copy after ring
publication. A dedicated long-running runtime does not need that profiling
copy; it is not part of the IPC cost above.

Pi artifacts:

```text
/home/cam/depthd-native-output-live-20260712/run.log
/home/cam/depthd-native-output-live-20260712/ring-inspect.txt
/home/cam/depthd-native-output-live-20260712/async_zero_copy_profile.json
/run/depthd/results.ring
```
