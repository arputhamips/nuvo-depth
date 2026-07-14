# EGL/OpenGL DMABUF rectification candidate

## Status and rollback

`egl_gl_dmabuf` is a production-candidate depth rectifier, default off. The
checked-in production configuration and systemd command still select
`cpu_opencv`; rollback is therefore either omitting the experimental command
line flags or setting `--rectification-backend cpu_opencv`.

No Vulkan rectification code is restored. This module implements the successful
direct path tested on the Pi:

```text
Picamera2 completed-request lease
  -> libcamera YUV420 Y-plane DMABUF metadata (no MappedArray)
  -> EGL_EXT_image_dma_buf_import as DRM_FORMAT_R8
  -> paired OpenGL ES 3 fragment remap with resident float maps
  -> one R8 CPU readback
  -> unchanged CPU SGBM
```

This is direct EGL/OpenGL, not a GStreamer pipeline. RGB888 DMABUF import is not
supported. Scheduled color rectification stays on `cpu_opencv`.

## Build

The module is isolated behind an option that defaults to `OFF`:

```bash
cmake -S . -B build \
  -DDEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION=ON \
  -DBUILD_TESTING=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

The enabled build requires Linux, OpenCV 4, pkg-config, EGL development files,
and OpenGL ES 3 development files (`egl` and `glesv2` pkg-config modules). It
produces `libdepthd_egl_gl_dmabuf_rectification.so`. With the option off, the
common cache/boundary unit tests remain dependency-free from EGL/GLES and the
normal CPU build does not search for either package.

Initialization also verifies that `GL_RENDERER` identifies V3D/VideoCore. A
Mesa software renderer is treated as an initialization failure and follows the
configured CPU fallback instead of silently consuming CPU under a GPU label.

## Selection

The production orchestrator has a real capture-native selection path:

```bash
python3 tools/pi/profile_async_zero_copy_pipeline.py \
  --left-calibration config/calibration/nuvo_reference/front_right.yaml \
  --right-calibration config/calibration/nuvo_reference/front_left.yaml \
  --output-dir /tmp/egl-gl-depth \
  --capture-stream-profile isp_luma_rgb_left \
  --rectification-backend egl_gl_dmabuf \
  --rectification-fallback-backend cpu_opencv \
  --allow-rectification-fallback \
  --egl-gl-rectification-library \
    build/libdepthd_egl_gl_dmabuf_rectification.so
```

`StereoPairLease.dmabuf_luma_views()` extracts fd, visible size, stride, plane
offset, implicit/explicit modifier, DMA allocation identity, and request id
while the completed requests remain leased. The fast path never enters
`MappedArray`. If native initialization/import/draw/readback fails and fallback
is enabled, the backend records the reason, releases all EGL/GL/duplicated-fd
resources, permanently selects `cpu_opencv`, and asks the Python adapter to map
that same still-live pair before retrying. This makes fallback lazy instead of
adding CPU DMA synchronization to every healthy GPU frame.

## Lifetime and cache rules

- Only full-resolution YUV420 Y planes are accepted as R8. Visible geometry is
  currently 728 x 544; the supplied row stride is validated and passed to EGL
  (the production PiSP layout is 768 bytes).
- The capture adapter derives a stable allocation id with `fstat`; native code
  repeats `fstat` and compares fd, device/inode, format, geometry, stride,
  offset, and modifier before every cache hit.
- The cache is a pre-reserved bounded vector (default eight imports for two
  four-buffer camera pools). A changed allocation is destroyed/re-imported;
  an additional allocation evicts the least-recent entry.
- EGLImages own duplicated close-on-exec fds. Cache eviction, GPU failure,
  construction failure, and destruction release texture, EGLImage, and fd in
  deterministic order.
- Maps, shaders, program, framebuffer, output texture, query object, and CPU
  readback storage are created once. The steady path does not compile shaders,
  upload maps, allocate descriptors, or allocate heap storage.

## Sampling and timing

Both camera maps are quantized once to OpenCV's 1/32-pixel `INTER_BITS=5`
coordinates. The shader bounds-checks each of four R8 neighbors, so
out-of-range contributions are constant zero. One draw covers the left/right
output side by side. The synchronous result is copied into caller-owned Gray8
rows for the unchanged CPU matcher.

Diagnostics report selected backend, fallback reason, cache size/capacity,
imports, stale re-imports, evictions, failures, owned fds, GPU-timer support,
input/compute/output/total time, and host submit/wait. `compute_ms` is an
`GL_EXT_disjoint_timer_query` result when supported and host draw/finish time
otherwise.

## Live comparison gate

Add `--compare-rectification-backends` to run `cpu_opencv` after the GPU call on
the same leased requests. The JSON report includes:

- exposure/gain and sensor-pair measurements already collected by the
  production profiler;
- exact/within-one pixel fractions, MAE, p95, and maximum error;
- downstream SGBM valid-mask agreement and p50/p95/max working-pixel error;
- a last-frame 5 cm camera-model/rig-transform spatial voxel comparison, plus
  the actual selected pipeline's native compact-cloud counts when native
  output is enabled;
- GPU input/compute/output/total and submit/wait distributions;
- cadence/deadline misses and process/whole-SoC CPU;
- cache/import/fd/fallback diagnostics and normal capture shutdown statistics.

The spatial voxel comparison is deliberately labeled a pre-support proxy. When
native output is enabled, comparison mode now also creates a private second
result ring and sends the CPU disparity through the real compact-cloud builder.
The report contains same-pair point, raw-point, voxel, supported-voxel, cluster,
and overflow distributions for both backends.

## Pi 5 live acceptance

Live acceptance was completed on CamMow with the frozen half-resolution SGBM
profile, PiSP luma capture, shared auto exposure, and CPU fallback disabled.
Production `/run/depthd` rings and the installed service were not used.

The 600-pair same-lease comparison is stored at:

```text
/home/cam/egl-gl-compare-89bd720-600c
```

| Gate | Result |
| --- | ---: |
| Output rate | 9.9945 Hz |
| Actual deadline misses | 0 |
| GPU total p50 / p95 | 4.256 / 4.277 ms |
| Pixels within one LSB / maximum error | 100% / 1 LSB |
| Exact pixels | 98.249% |
| Disparity difference p50 / p95 | 0.000 / 0.375 working px |
| AE control mismatches | 0 of 600 decisions |
| AE maximum shutter / gain delta | 0 us / 0.0x |
| Compact points p50, GPU / CPU | 426 / 427 |
| Supported voxels p50, GPU / CPU | 526 / 525 |
| Accepted clusters p50, GPU / CPU | 8 / 8 |
| Compact-point absolute delta p50 / p95 | 7 / 21 points |
| Voxel capacity overflow, GPU / CPU | 0 / 0 |
| Cache size / capacity / owned fds | 8 / 8 / 8 |
| Imports / failures / fallbacks | 8 / 0 / 0 |

`deadline_misses` counts missing sensor periods or overwritten ready pairs.
Pacing-loop samples slightly above 100 ms are retained separately as
`cycle_over_period_samples`; they are not frame loss when the following camera
wait contracts and sensor timestamps remain consecutive.

The full 600-pair GPU + native cloud + 2 Hz JPEG + asynchronous Hailo semantic
run is stored at:

```text
/home/cam/egl-gl-full-semantic-89bd720-600a
```

It produced 9.9988 Hz with zero deadline misses. GPU total was 4.301/4.460 ms
p50/p95. The process used 11.63% of total four-core capacity. The semantic
worker published 251 results with zero errors or pending-frame replacements and
released all 251 submitted leases during clean shutdown.

Ten additional independent start/stop cycles are stored under:

```text
/home/cam/egl-gl-restarts-89bd720
```

All ten selected V3D `egl_gl_dmabuf`, reported zero deadline misses and zero
backend failures, held exactly eight cached imports/eight owned fds, emitted no
allocator `BufferError` or traceback, and left no profiler process running.

The live gates pass. The checked-in selection intentionally remains
`cpu_opencv`; `egl_gl_dmabuf` is an accepted default-off module until a separate
deployment decision changes the production profile.

## Existing feasibility evidence

The temporary direct-GL prototype measured 1.286/1.488 ms p50/p95 shader time
and 2.183/2.219 ms complete paired rectification plus CPU readback. Pixel exact
fractions were 98.28%/98.48%, all pixels were within one LSB, downstream
valid-mask agreement was 96.36%, and disparity mean/p95 differences were
0.215/0.438 working pixels. These numbers motivate the backend but are not a
substitute for the integrated 600-pair acceptance run.
