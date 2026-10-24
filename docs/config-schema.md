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
[shared stereo exposure control](user-guide.md#exposure-and-flicker).

Native auto exposure uses the YAML `metering_roi` pixel rectangle
(`[x_min, y_min, x_max, y_max]`, upper bounds excluded), `dark_threshold`,
`clipping_threshold`, and `max_clipped_fraction`. These are profile settings;
the existing 15-field `exposure.json` schema is unchanged. Metering uses a
stride-four luma histogram in each camera, with the worse camera's clipped
fraction and a shared median. It does not allocate image-sized sample vectors.

The controller filters median brightness in log space and requires three
consecutive observations in the same direction. Adjustments request at most
0.2 EV per step (subject to the 50 µs shutter and 0.05 gain quantization).
It prefers longer shutter to additional gain within the configured limits.
Clipping selects the lower end of the median deadband; it cannot repeatedly
halve exposure below that band to chase a small saturated highlight. An
adjustment that encounters clipping retains the lower target until convergence;
crossing the clipping threshold on adjacent shutter steps cannot alternate the
target. Once an acceptable value or a control limit is reached, exposure holds
until median brightness changes by more than 20% for three observations, with
a filtered 15% threshold as well. The held baseline does not drift, so gradual lighting
changes still trigger an adjustment. This policy deliberately permits small
highlights to remain saturated when preserving usable midtones requires it.

When capture supplies applied-control metadata, both cameras' reported shutter
and analogue gain must match the command before the settling countdown starts.
The shutter tolerance is the larger of 1% or 25 µs. Gain tolerance is 1.25%,
covering the IMX296's approximately 1.16% gain-code step. Missing metadata
reports `applied_metadata_unavailable`; mismatches
report `awaiting_applied_controls`. Both hold automatic adjustments and retain
the last command, while status reports the wait-frame count. This is an
acknowledgement of reported sensor controls, not a measurement of physical XTR
pulse timing. Callers without applied metadata retain frame-count settling.
After matching, `auto_settle_frames_after_update` frames are still excluded
from metering. The update interval controls observation cadence, so response
time depends on capture rate as well as the configured interval.

## Scope

The native configuration component parses
and validates the current `schema_version: 1` product profile without opening a
camera, GPIO line, EGL device, calibration file, or any path under
`/var/lib`.

The installed native service runs this preflight before constructing capture,
rectification, matching and output stages from the normalized settings.

Schema v1 remains an import boundary so the immutable seed and existing device
configurations keep loading. Active fields normalize into `EffectiveConfig`.
Retired ROS topic, obstacle-cloud and result-ring keys are accepted as optional
compatibility keys, but are not stored, validated as runtime settings, included
in effective JSON, or included in the configuration digest. Unknown and duplicate
keys still fail preflight. Native geometry, calibration and capture validity
checks remain enforced.

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
- active output cadence, metric-depth and encoder fallback settings.

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
  disparity_hz, metric_depth_enabled, compressed_image_hz, jpeg_quality
```

The frozen schema-v1 seed also contains optional, ignored output keys:
`disparity_topic`, `compressed_image_topic`, `left_camera_info_topic`,
`diagnostics_topic`, `obstacle_cloud_enabled`, `obstacle_cloud_topic`,
`result_ring_path`, `result_ring_slots`, and `result_ring_left_mono_enabled`.
Their presence or values do not change the native effective configuration.

## Structural validation

The preflight rejects:

- unsupported schema versions, missing/unknown/duplicate keys, wrong node
  shapes, integer range errors, NaN, and infinity;
- zero, excessive, or overflowing frame/buffer geometry;
- duplicate logical camera indices and unsupported schema-v1 mode identifiers;
- invalid orientation, buffer count, calibration paths or trigger GPIO paths;
- a trigger pulse, exposure, or pair tolerance inconsistent with capture
  cadence;
- invalid metering ROI, thresholds, clipped fraction, shutter range, gain range,
  or initial exposure/gain;
- mismatched rig/stereo baselines, capture/output geometry, or aspect ratio;
- invalid OpenCV disparity multiples, search span, block size, prefilter cap, or
  thread count;
- output cadences above capture cadence or invalid JPEG quality.

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

## Runtime network-product permission

`/etc/nuvo-depth/transport.json` is separate from capture/matcher YAML and from
OS Wi-Fi configuration. Its schema-v1 fields are:

```json
{"schema_version": 1, "preference": "auto", "wifi_enabled": true}
```

`preference` retains `auto`, `ethernet`, `usb`, `both`, `wifi` and `off`.
`wifi_enabled` is optional and must be a JSON boolean. When absent, legacy
behavior is preserved: `auto` and `wifi` permit Wi-Fi; the other preferences do
not. Once explicitly set, Wi-Fi permission is independent of wired preference.
`off` always blocks every product transport, even with `wifi_enabled: true`.
The configured permission and effective permission are therefore distinct.

`GET /api/v1/transport` returns the configured or legacy-derived Wi-Fi setting,
the current preference, and `connection.products_permitted` for the receiving
camera interface after applying the master policy. `POST /api/v1/transport/stage` accepts a partial change
such as `{"wifi_enabled": false}` without rewriting the wired preference.
Existing explicit Wi-Fi permission survives a preference-only change; an old
document with no explicit flag continues to follow legacy preference semantics.
Confirm or roll back the staged policy within 60 seconds. Rollback restores both
the prior preference and whether the Wi-Fi flag was originally omitted.
The transport response also exposes pending transaction status/deadline so a
reloaded console can confirm or undo it. A second unconfirmed change is rejected
until the first is confirmed, undone or reconciled after expiry.

The gateway applies permission to compressed and uncompressed product endpoints
and active sessions. Console/control access is retained. Invalid policy files
block product delivery rather than enabling a fallback link. A policy change
does not move a TCP connection between Ethernet, USB Ethernet and Wi-Fi.
