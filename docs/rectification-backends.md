# Rectification backends

## Boundary

Rectification is independent from stereo matching. It consumes oriented camera
views plus immutable inverse-remap coordinates and produces rectified views in
the same pixel encoding. The selected `StereoBackend` always receives images
through this boundary, regardless of whether EGL/GLES or OpenCV is selected.

The common API is `include/depthd/rectification_backend.hpp`. The OpenCV module
registers its factory explicitly in a caller-owned
`RectificationBackendRegistry`; it does not use static initialization.

The locked runtime configuration is:

```yaml
rectification:
  backend: egl_gl_dmabuf
  fallback_backend: cpu_opencv
  allow_fallback: true
```

## Data contract

- `RectificationMap` owns dense float32 source X/Y coordinates, one coordinate
  pair per output pixel.
- `rectify()` handles one left or right image. This is used for the throttled
  full-color left output.
- `rectify_pair()` handles the two grayscale depth inputs.
- Input and output use caller-owned `ImageView`/`MutableImageView` buffers.
  Rectification preserves `Gray8`, `Bgr8`, or `Rgb8`; it never converts color.
- Input and output may have row padding but may not alias. The backend may not
  retain caller buffers after a synchronous call returns.
- Linear sampling and a constant zero border are the production semantics.
- Map conversion and reusable allocations belong to backend construction, not
  the per-frame path.

## Production implementation

`cpu_opencv` converts the common float maps once to OpenCV fixed-point maps and
uses `cv::remap` with linear interpolation. The measured grayscale pair costs
5.35 ms p50 on the supplied Pi 5. The separately scheduled full-color left
rectification costs 6.77 ms p50 at 2 Hz.

The CPU implementation remains installed, is the automatic runtime fallback,
and is selectable with one configuration value. Its identity, subpixel,
border, padded-stride, grayscale, BGR, and RGB behavior remains under regression
testing. Capture, camera ordering, calibration mapping, and the locked SGBM
production profile do not depend on the selected accelerator.

## Production capture-native GPU backend

`egl_gl_dmabuf` is the selected direct EGL/OpenGL ES backend for paired YUV420
luma only. It extends the boundary with explicit DMABUF plane and request-lease
metadata without treating an fd as an `ImageView`; the host-buffer CPU API is
unchanged. Build, selection, cache/lifetime rules, diagnostics, comparison
mode, rollback, live equivalence, and complete-pipeline evidence are documented in
`docs/egl-gl-dmabuf-rectification.md`.
