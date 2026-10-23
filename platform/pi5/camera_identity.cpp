#include "depthd/capture.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace depthd {

void require_trigger_source(const CaptureOpenOptions& options,
                            const bool hardware_timed_source_available) {
  if (options.hardware_trigger_enabled && !hardware_timed_source_available &&
      !options.allow_userspace_trigger) {
    throw std::runtime_error(
        "hardware-timed PIO trigger unavailable; run nuvo-trigger-prepare before capture. "
        "Userspace GPIO is available only with capture-check --allow-userspace-trigger");
  }
}

void validate_negotiated_stream(const CaptureOpenOptions& requested,
                                const NegotiatedCameraStream& negotiated) {
  const auto channels = requested.pixel_format == "RGB888" ||
                        requested.pixel_format == "BGR888" ? 3U : 1U;
  const auto minimum_stride = static_cast<std::uint64_t>(requested.width) * channels;
  if (negotiated.grid.width != requested.width ||
      negotiated.grid.height != requested.height ||
      negotiated.pixel_format != requested.pixel_format ||
      negotiated.buffer_count != requested.buffer_count ||
      negotiated.stride_bytes < minimum_stride) {
    throw std::runtime_error(
        "camera negotiated a different geometry, pixel format, stride, or buffer count");
  }
}
namespace {

bool valid_role(const CameraRole role) noexcept {
  return role == CameraRole::kLogicalLeft ||
         role == CameraRole::kLogicalRight;
}

}  // namespace

void validate(const CameraIdentity& identity) {
  if (identity.stable_id.empty()) {
    throw std::invalid_argument("camera identity requires a stable platform ID");
  }
  if (identity.model.empty()) {
    throw std::invalid_argument("camera identity requires a model name");
  }
}

void canonicalize_camera_order(std::vector<CameraIdentity>& cameras) {
  for (const auto& camera : cameras) {
    validate(camera);
  }
  std::sort(cameras.begin(), cameras.end(),
            [](const CameraIdentity& left, const CameraIdentity& right) {
              return left.stable_id < right.stable_id;
            });
  for (std::size_t index = 0; index < cameras.size(); ++index) {
    if (index != 0U &&
        cameras[index - 1U].stable_id == cameras[index].stable_id) {
      throw std::invalid_argument("camera discovery contains duplicate physical IDs");
    }
    cameras[index].discovery_index = static_cast<std::uint32_t>(index);
  }
}

void validate(const CameraBinding& binding) {
  if (!valid_role(binding.role)) {
    throw std::invalid_argument("camera binding has an unknown logical role");
  }
  validate(binding.camera);
}

std::array<CameraBinding, 2> bind_pi5_fixed_csi_ports(
    const std::vector<CameraIdentity>& cameras) {
  const auto select = [&](const std::string_view port, const CameraRole role) {
    const CameraIdentity* selected = nullptr;
    for (const auto& camera : cameras) {
      validate(camera);
      if (!camera.stable_id.ends_with(port)) continue;
      if (selected != nullptr) {
        throw std::runtime_error("multiple cameras match fixed Pi5 CSI port " +
                                 std::string(port));
      }
      selected = &camera;
    }
    if (selected == nullptr) {
      throw std::runtime_error("required Pi5 CSI camera is missing: " +
                               std::string(port));
    }
    return CameraBinding{role, *selected};
  };
  std::array<CameraBinding, 2> bindings{
      select("/rp1/i2c@80000/imx296@1a", CameraRole::kLogicalLeft),
      select("/rp1/i2c@88000/imx296@1a", CameraRole::kLogicalRight)};
  validate_stereo_bindings(bindings[0], bindings[1]);
  return bindings;
}

void validate_stereo_bindings(const CameraBinding& left,
                              const CameraBinding& right) {
  validate(left);
  validate(right);
  if (left.role != CameraRole::kLogicalLeft ||
      right.role != CameraRole::kLogicalRight) {
    throw std::invalid_argument(
        "stereo bindings must be ordered logical-left then logical-right");
  }
  if (left.camera.stable_id == right.camera.stable_id) {
    throw std::invalid_argument(
        "logical left and right cannot bind the same physical camera");
  }
}

void validate(const CaptureOpenOptions& options) {
  if (options.left_camera_index == options.right_camera_index) {
    throw std::invalid_argument("capture camera indices must differ");
  }
  if (options.width == 0U || options.height == 0U) {
    throw std::invalid_argument("capture dimensions must be non-zero");
  }
  if (static_cast<std::uint64_t>(options.width) * options.height >
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::invalid_argument("capture pixel count exceeds supported range");
  }
  if (options.pixel_format.empty()) {
    throw std::invalid_argument("capture pixel format is empty");
  }
  if (options.orientation_degrees != 0U &&
      options.orientation_degrees != 180U) {
    throw std::invalid_argument(
        "Pi capture orientation must currently be 0 or 180 degrees");
  }
  if (options.buffer_count < 2U || options.buffer_count > 64U) {
    throw std::invalid_argument("capture buffer count must be between 2 and 64");
  }
  if (!std::isfinite(options.target_hz) || options.target_hz <= 0.0 ||
      options.target_hz > 1'000.0) {
    throw std::invalid_argument("capture cadence must be finite and positive");
  }
  if (options.pair_tolerance_ns >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("capture pair tolerance exceeds timestamp range");
  }
  if (!std::isfinite(options.analogue_gain) || options.analogue_gain <= 0.0) {
    throw std::invalid_argument("capture analogue gain must be finite and positive");
  }
  if (std::any_of(options.colour_gains.begin(), options.colour_gains.end(),
                  [](const double gain) {
        return !std::isfinite(gain) || gain <= 0.0;
      })) {
    throw std::invalid_argument("capture colour gains must be finite and positive");
  }
  if (options.exposure_time_us == 0U) {
    throw std::invalid_argument("capture exposure time must be non-zero");
  }
  if (options.hardware_trigger_enabled) {
    if (options.trigger_driver_mode != "external_xtrig") {
      throw std::invalid_argument(
          "Pi capture supports only the external_xtrig trigger mode");
    }
    if (options.gpiochip.empty()) {
      throw std::invalid_argument("trigger gpiochip path is empty");
    }
    if (options.trigger_pulse_width_us == 0U) {
      throw std::invalid_argument("trigger pulse width must be non-zero");
    }
    const auto period_us = 1'000'000.0 / options.target_hz;
    if (static_cast<double>(options.trigger_pulse_width_us) +
            static_cast<double>(kExternalTriggerGuardUs) >
        period_us) {
      throw std::invalid_argument(
          "trigger pulse width must leave the required capture-period guard");
    }
  }
}

void validate(const CaptureControls& controls) {
  if (controls.exposure_time_us < 250U || controls.exposure_time_us > 8'000U) {
    throw std::invalid_argument(
        "capture exposure must be between 250 and 8000 microseconds");
  }
  if (!std::isfinite(controls.analogue_gain) ||
      controls.analogue_gain < 1.0 || controls.analogue_gain > 3.0) {
    throw std::invalid_argument(
        "capture analogue gain must be between 1x and 3x");
  }
  if (std::any_of(controls.colour_gains.begin(), controls.colour_gains.end(),
                  [](const double gain) {
                    return !std::isfinite(gain) || gain < 0.1 || gain > 16.0;
                  })) {
    throw std::invalid_argument(
        "capture colour gains must be between 0.1x and 16x");
  }
}

FrameSetIdentity make_frame_set_identity(
    const StereoPair& pair, const std::array<std::uint8_t, 16>& device_uuid,
    const std::array<std::uint8_t, 16>& run_uuid,
    const std::uint64_t configuration_epoch) {
  if (pair.capture_epoch == 0U || pair.pair_sequence == 0U ||
      pair.capture_timestamp_ns <= 0 ||
      pair.left.role != CameraRole::kLogicalLeft ||
      pair.right.role != CameraRole::kLogicalRight ||
      pair.left.capture_epoch != pair.capture_epoch ||
      pair.right.capture_epoch != pair.capture_epoch ||
      pair.left.sensor_clock != CaptureClock::kBoottime ||
      pair.right.sensor_clock != CaptureClock::kBoottime) {
    throw std::invalid_argument(
        "stereo pair cannot form a coherent product identity");
  }

  FrameSetIdentity identity;
  identity.device_uuid = device_uuid;
  identity.run_uuid = run_uuid;
  identity.capture_epoch = pair.capture_epoch;
  identity.capture_sequence = pair.pair_sequence;
  identity.configuration_epoch = configuration_epoch;
  identity.capture_time_ns = pair.capture_timestamp_ns;
  identity.capture_clock = CaptureClock::kBoottime;
  validate(identity);
  return identity;
}

}  // namespace depthd
