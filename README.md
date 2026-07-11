# Depth Daemon

Native stereo-depth runtime for the Nuvo Raspberry Pi 5, dual hardware-triggered
IMX296 cameras, and Hailo-8L. The hot path is intentionally independent of ROS:
capture, pair validation, rectification, inference, and buffer management happen
outside DDS. A small boundary adapter publishes only finished products.

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
[LAS2-to-HEF gate plan](docs/model-porting.md).

## Repository shape

```text
include/depthd/       Stable ROS-free data contracts
src/                  ROS-free implementation
tests/                Dependency-light contract tests
config/               Runtime profile and reference calibration
docs/                 Decisions, audit notes, and acceptance criteria
models/               Model provenance and conversion instructions
third_party/          Pinned upstream sources (git submodules)
```

The runtime, camera backend, Hailo backend, IPC transport, and ROS bridge will be
added after the deployment architecture is selected. The core will remain a
normal CMake project; the bridge will be the only ROS/ament package.

## Build the current contract test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Non-goals

- No raw left/right ROS image loop.
- No Python in the Pi frame loop.
- No full-rate dense `PointCloud2`.
- No claim that LAS2 compiles for Hailo-8L until the compiler and hardware gates
  pass.
