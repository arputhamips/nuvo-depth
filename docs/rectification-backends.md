# Switchable rectification backends

## Boundary

Rectification is independent from stereo matching. It consumes oriented camera
views plus immutable inverse-remap coordinates and produces rectified views in
the same pixel encoding. The selected `StereoBackend` continues to receive
already-rectified images and does not need to know whether OpenCV or Vulkan
performed the remap.

The common API is `include/depthd/rectification_backend.hpp`. Optional modules
register factories in a caller-owned `RectificationBackendRegistry`; no module
uses static initialization and no optional GPU dependency leaks into the common
target.

The runtime configuration is:

```yaml
rectification:
  backend: cpu_opencv
  fallback_backend: cpu_opencv
  allow_fallback: true
```

The initial implementation registers `cpu_opencv`. The Vulkan module must
register `vulkan_compute`. Selecting an unavailable backend either creates the
explicit fallback or fails with an actionable error, depending on
`allow_fallback`.

## Data contract

- `RectificationMap` owns dense float32 source X/Y coordinates, one coordinate
  pair per output pixel. Both CPU and GPU implementations consume the same
  calibrated coordinates.
- `rectify()` handles one left or right image. This is used for the throttled
  full-color left output.
- `rectify_pair()` handles the two grayscale depth inputs. Its default
  implementation calls `rectify()` twice; a GPU backend may override it to
  batch both cameras in one submission.
- Input and output use caller-owned `ImageView`/`MutableImageView` buffers.
  Rectification preserves `Gray8`, `Bgr8`, or `Rgb8`; it never converts color.
- Input and output may have row padding but may not alias. A backend may not
  retain caller buffers after a synchronous call returns.
- Linear sampling and a constant zero border are the current production
  semantics.
- Map upload, pipelines, descriptors, command buffers, and reusable staging
  allocations belong to backend construction, not the per-frame path.

`RectificationStats` deliberately separates input transfer, compute, output
transfer, and total latency. A shader-only speedup is not useful if CPU-visible
readback makes the complete operation slower: CPU SGBM consumes the rectified
grayscale images immediately afterward.

## Current CPU reference

`cpu_opencv` converts the common float maps once to OpenCV fixed-point maps and
uses `cv::remap` with linear interpolation. The measured grayscale pair costs
5.35 ms p50 on the supplied Pi 5. The separately scheduled full-color left
rectification costs 6.77 ms p50 at 2 Hz.

## Vulkan module constraints

The Vulkan implementation should be isolated to its own header/source, shader,
tests, benchmark, and optional CMake target. It should not change capture,
camera ordering, calibration mapping, SGBM, ROS, or the common rectification
interface.

Required behavior:

1. Add `DEPTHD_WITH_VULKAN_RECTIFICATION`, default `OFF`.
2. When enabled, locate Vulkan and a GLSL-to-SPIR-V compiler with clear failure
   messages; default CPU-only builds must remain dependency-free.
3. Register the name `vulkan_compute` explicitly with the common registry.
4. Load/upload both coordinate maps once and reuse all steady-state resources.
5. Support one-channel grayscale and three-channel BGR/RGB with arbitrary valid
   row strides.
6. Implement bilinear sampling with constant-zero border behavior matching the
   CPU reference, including edge coordinates.
7. Prefer a paired grayscale dispatch, but keep the single-image path for the
   2 Hz color output.
8. Use a bounded synchronous adapter initially. No per-frame device creation,
   pipeline compilation, descriptor allocation, or unbounded queueing.
9. Report host copies/mapping, GPU execution, synchronization/readback, and
   total time separately.
10. Treat DMABUF import as a later optimization. The first implementation must
    be correct with reusable host-visible/staging buffers before adding camera
    allocator coupling.

## Acceptance

- Registry fallback and default CPU-only builds/tests remain green.
- Identity, subpixel, border, padded-stride, grayscale, BGR, and RGB cases are
  tested against `cpu_opencv`.
- Rectified intensity differences and downstream SGBM disparity differences are
  reported, not only visualized.
- A 600-pair robot run maintains 10 Hz with no capture replacements/rejections.
- Report p50/p95/max upload, shader, readback, total rectification, complete
  depth-ready latency, process CPU, whole-SoC CPU, RSS, temperature, and thermal
  flags.
- The GPU backend is considered useful only if total grayscale-pair
  rectification is below the 5.35 ms CPU reference and aggregate process CPU is
  lower after readback into CPU SGBM.
