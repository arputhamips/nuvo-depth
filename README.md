# Depth Daemon

Native stereo-depth runtime for the Nuvo Raspberry Pi 5 and dual
hardware-triggered IMX296 cameras. Stereo processing is selected through a
ROS-free backend interface; the first working backend is OpenCV CPU SGBM, with
Vulkan and Hailo backends able to implement the same contract later. Capture,
pair validation, rectification, matching, and buffer management happen outside
DDS. A small boundary adapter publishes only finished products.

This initial commit is decision-ready rather than hardware-complete. It records
the existing Nuvo contract, pins the LAS2 investigation, and isolates the small
stable C++ contract from the still-open deployment choice.

## Fixed compatibility target

- Capture/output geometry: **728 x 544** at **6-10 Hz**.
- Current camera profile: RGB888, 180-degree orientation, 10 Hz shared hardware
  trigger on GPIO 17.
- Rectified focal length: 493.542046188 px.
- Calibrated baseline: 0.161458780 m.
- Required ROS data: `/camera/front/disparity` plus a throttled JPEG view.
- Required support data: left camera info and health/latency diagnostics.
- Future extension: compact voxelized `/camera/front/obstacles`; no dense cloud
  in the default path.

See [architecture options](docs/architecture-options.md), the exact
[Nuvo compatibility contract](docs/nuvo-compatibility.md), and the
[LAS2-to-HEF gate plan](docs/model-porting.md). The first measured runtime is
documented in the [CPU SGBM baseline](docs/cpu-sgbm-baseline.md), with the full
live camera path in the [CPU pipeline profile](docs/cpu-live-pipeline-profile.md)
and the corrected [async zero-copy trigger results](docs/async-zero-copy-trigger-results.md).

Image rectification has its own switchable backend registry, separate from
stereo matching. The default `cpu_opencv` implementation and optional GPU
module contract are documented in the
[rectification backend guide](docs/rectification-backends.md).

## Repository shape

```text
include/depthd/       Stable ROS-free data contracts
src/                  ROS-free backend implementations
tests/                Dependency-light contract tests
config/               Runtime profile and reference calibration
docs/                 Decisions, audit notes, and acceptance criteria
models/               Model provenance and conversion instructions
third_party/          Pinned upstream sources (git submodules)
```

The camera runtime, Vulkan/Hailo backends, IPC transport, and ROS bridge remain
to be added. The core is a normal CMake project; the bridge will be the only
ROS/ament package.

## Build the current contract test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

When OpenCV development packages are present, this also builds
`depthd_sgbm_benchmark`. It accepts a rectified 728 x 544 pair and reports
per-stage latency plus process and normalized four-core CPU utilization.

## Non-goals

- No raw left/right ROS image loop.
- No Python in the Pi frame loop.
- No full-rate dense `PointCloud2`.
- No claim that LAS2 compiles for Hailo-8L until the compiler and hardware gates
  pass.
