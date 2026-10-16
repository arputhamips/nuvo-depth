# Native configuration preflight

> The YAML profile below remains the immutable appliance/capture baseline.
> Three small live-control schemas layer on top of it: `encoding.json`,
> `stereo.json`, and `exposure.json`. Each is a closed schema-v1 object,
> atomically updated through `nuvoctl`, and reloaded without restarting capture.

The exposure control schema contains `mode` (`manual` or `auto`),
`manual_exposure_us`, `manual_analogue_gain`, `red_gain`, `blue_gain`, target
and deadband medians, auto exposure/gain limits, update interval, and settle
frames. Schema bounds are 250–8,000 µs and 1–3× analogue gain; runtime further
limits XTR exposure to `trigger period - 250 µs` without changing schema v1. See
[shared stereo exposure control](stereo-auto-exposure.md).

## Scope

The native configuration component is an opt-in preflight skeleton. It parses
and validates the current `schema_version: 1` product profile without opening a
camera, GPIO line, EGL device, result ring, calibration file, or any path under
`/var/lib`.

It is not the production frame loop and is not used by the installed systemd
service yet.

Schema v1 is treated as a legacy import boundary. Every current field is parsed
into `LegacyProfileV1`, then normalized into `EffectiveConfig`. ROS topic fields
are retained under `legacy_ros_adapter`; they do not define native daemon
behavior. This lets a later schema replace historical ROS and fixed-profile
concepts without making them part of the permanent daemon API.

## Build

Configuration support is disabled by default so the repository's existing
dependency-light build remains available:

```bash
cmake -S . -B build -DDEPTHD_WITH_CONFIG=OFF
```

Enable the preflight explicitly when the system `yaml-cpp` development package
is available:

```bash
cmake -S . -B build \
  -DDEPTHD_WITH_CONFIG=ON \
  -DDEPTHD_WITH_OPENCV=OFF \
  -DDEPTHD_WITH_SEMANTICS=OFF \
  -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

An enabled configuration build fails during CMake configuration if `yaml-cpp`
is absent or does not export a supported CMake target. It never downloads a
dependency implicitly. A future Pi release preset will require this option;
the current package and systemd unit are deliberately unchanged in this step.

## Commands

Structural validation with human-readable output:

```bash
./build/depthd --check-config config/nuvo_pi5_depth_100mm.yaml
```

Deterministic machine-readable output:

```bash
./build/depthd \
  --dump-effective-config config/nuvo_pi5_depth_100mm.yaml
```

The JSON dump includes:

- effective and source schema versions;
- SHA-256 of the canonical effective configuration object;
- structured warnings;
- the normalized configuration;
- legacy ROS adapter fields in their own object.

The hash does not depend on the input filename, YAML comments, mapping order,
or number spelling. It is computed over the canonical effective configuration,
excluding warnings and the hash itself.

Exit status is `0` for a structurally valid configuration, `2` for usage or
validation errors, `3` for file I/O errors, and `70` for unexpected internal
errors. Validation diagnostics name the logical YAML path.

## Schema-v1 keys

The Pi 5 product binds logical left to the IMX296 on RP1 `i2c@80000` and
logical right to RP1 `i2c@88000`. These fixed CSI port paths determine roles;
discovery order, extra cameras and configurable list indices cannot swap them.
A missing or ambiguous required port prevents capture instead of selecting
another sensor. The legacy schema indices remain for compatibility and describe
left 0/right 1 in the sorted dual-camera inventory. The provisional seed keeps
each sensor's intrinsics, rectification and historical output correction with
its physical port. Rewiring requires updating these bindings and recalibrating.

All mappings are closed: listed keys are required except the optional
`stereo.median_filter_size`, and unknown or duplicate keys are errors.

```text
schema_version
product
  platform, role, profile_id
rig
  sensor, camera_count, baseline_m, calibration_status
capture
  backend, left_camera_index, right_camera_index, width, height
  stream_profile, main_pixel_format, depth_plane
  left_color_stream, left_color_pixel_format, left_color_memory_order
  right_color_stream_enabled, colour_space, orientation_degrees
  target_hz, buffer_count, handoff, zero_copy_camera_input
  hardware_trigger
    enabled, driver_mode, requires_always_on_overlay, gpiochip, line
    idle_level, active_level, pulse_width_us, startup_priming_pulses
    pair_tolerance_us
  exposure_time_us, analogue_gain, colour_gains
  auto_exposure
    enabled, metering_roi, target_median, median_deadband
    dark_threshold, clipping_threshold, max_clipped_fraction
    min_shutter_us, max_shutter_us
    min_analogue_gain, max_analogue_gain
    update_interval_frames, settle_frames_after_update
rectification
  backend, fallback_backend, allow_fallback
  left_calibration, right_calibration, map_format
  depth_preprocessing, color_rectification
stereo
  backend, baseline_m, rectified_focal_length_px
  output_width, output_height, min_disparity, max_disparity
  working_width, working_height, working_disparities, block_size, mode
  uniqueness_ratio, speckle_window_size, speckle_range
  disparity_12_max_diff, pre_filter_cap, opencv_threads
outputs
  disparity_hz, disparity_topic, metric_depth_enabled
  compressed_image_hz, compressed_image_topic, jpeg_quality
  left_camera_info_topic, diagnostics_topic
  obstacle_cloud_enabled, obstacle_cloud_topic
  result_ring_path, result_ring_slots, result_ring_left_mono_enabled
```

## Structural validation

The preflight rejects:

- unsupported schema versions, missing/unknown/duplicate keys, wrong node
  shapes, integer range errors, NaN, and infinity;
- zero, excessive, or overflowing frame/buffer geometry;
- duplicate logical camera indices and unsupported schema-v1 mode identifiers;
- invalid orientation, buffer count, POSIX runtime paths, or ROS topic syntax;
- a trigger pulse, exposure, or pair tolerance inconsistent with capture
  cadence;
- invalid metering ROI, thresholds, clipped fraction, shutter range, gain range,
  or initial exposure/gain;
- mismatched rig/stereo baselines, capture/output geometry, or aspect ratio;
- invalid OpenCV disparity multiples, search span, block size, prefilter cap, or
  thread count;
- output cadences above capture cadence, invalid JPEG quality, or invalid ring
  slot counts.

The preflight intentionally does not check that configured files exist. Package
asset verification and hardware runtime preflight remain separate operations.

## Provisional calibration and trigger warning

The current unmodified 100 mm profile is structurally valid. Its effective
output retains:

```text
calibration_status = provisional_recalibration_required
```

Successful validation emits a warning that this status does not authorize a
factory-calibrated metric-depth claim.

The configured IMX296 trigger-low interval is also only a requested structural
value. This command does not prove GPIO waveform timing, applied sensor
exposure, pair synchronization, or the hardware-specific relationship between
the low interval and exposure. Those checks belong to the future platform
runtime preflight and hardware acceptance test.

## Runtime stereo smoothing

The YAML fallback accepts `stereo.mode` as `sgbm_3way`, `sgbm_standard`,
`sgbm_hh4`, or `sgbm_hh`. Optional `stereo.median_filter_size` accepts `0`, `3`,
or `5` and defaults to `0` in existing schema-v1 documents. The shipped YAML
selects 3-way and median 0, matching the balanced controls before any runtime
control file is loaded.

The strict `stereo.json` runtime control accepts `sgbm_mode` as `3way`,
`standard`, `hh4`, or `hh`, and `median_filter_size` as `0`, `3`, or `5`.
The mode controls how many SGBM path directions are aggregated; more paths
reduce directional streaks at higher CPU and memory cost. The median is applied
only to matcher-valid subpixel disparity, and the original invalid mask is
retained so smoothing cannot manufacture coverage. These controls are
applied between frames without changing capture geometry or the calibration
preset identity. The browser reads exact profile controls from
`web/depth-profiles.json`; a regression check prevents its balanced default from
drifting from the JSON control file and YAML fallback. All profiles remain
unqualified candidates until the shipping hardware measurements pass.
