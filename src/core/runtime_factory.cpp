#include "depthd/runtime_factory.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

#include "depthd/opencv_rectification_backend.hpp"
#include "depthd/stereo_backend.hpp"

#if defined(DEPTHD_RUNTIME_WITH_EGL_GL)
#include "depthd/egl_gl_dmabuf_rectification_backend.hpp"
#endif

namespace depthd {
namespace {

[[nodiscard]] std::array<std::uint8_t, 16> uuid_from_hex(
    const std::string_view value, const std::size_t offset) {
  const auto hex = [](const char digit) -> std::uint8_t {
    if (digit >= '0' && digit <= '9') {
      return static_cast<std::uint8_t>(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f') {
      return static_cast<std::uint8_t>(digit - 'a' + 10);
    }
    throw std::invalid_argument("configuration digest is not lowercase hex");
  };
  if (value.size() < offset + 32U) {
    throw std::invalid_argument("configuration digest is too short");
  }
  std::array<std::uint8_t, 16> result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::uint8_t>(
        (hex(value[offset + index * 2U]) << 4U) |
        hex(value[offset + index * 2U + 1U]));
  }
  // RFC 4122 variant/version bits make logs and client displays predictable;
  // identity comparison still uses all 16 bytes.
  result[6] = static_cast<std::uint8_t>((result[6] & 0x0fU) | 0x50U);
  result[8] = static_cast<std::uint8_t>((result[8] & 0x3fU) | 0x80U);
  return result;
}

[[nodiscard]] std::array<std::uint8_t, 16> random_uuid() {
  std::random_device source;
  std::array<std::uint8_t, 16> result{};
  for (auto& byte : result) {
    byte = static_cast<std::uint8_t>(source());
  }
  result[6] = static_cast<std::uint8_t>((result[6] & 0x0fU) | 0x40U);
  result[8] = static_cast<std::uint8_t>((result[8] & 0x3fU) | 0x80U);
  return result;
}

[[nodiscard]] std::array<std::uint8_t, 16> parse_uuid(
    const std::string_view value) {
  std::string compact;
  compact.reserve(32U);
  for (const auto character : value) {
    if (character != '-') compact.push_back(character);
  }
  if (compact.size() != 32U) {
    throw std::invalid_argument("device UUID must contain 32 hexadecimal digits");
  }
  const auto hex = [](const char digit) -> std::uint8_t {
    if (digit >= '0' && digit <= '9') return static_cast<std::uint8_t>(digit - '0');
    if (digit >= 'a' && digit <= 'f') return static_cast<std::uint8_t>(digit - 'a' + 10);
    if (digit >= 'A' && digit <= 'F') return static_cast<std::uint8_t>(digit - 'A' + 10);
    throw std::invalid_argument("device UUID contains a non-hexadecimal digit");
  };
  std::array<std::uint8_t, 16> result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::uint8_t>(
        (hex(compact[index * 2U]) << 4U) | hex(compact[index * 2U + 1U]));
  }
  if ((result[8] & 0xc0U) != 0x80U) {
    throw std::invalid_argument("device UUID does not use the RFC 4122 variant");
  }
  return result;
}

[[nodiscard]] bool near(const double left, const double right) {
  return std::abs(left - right) <=
         std::max(1e-8, std::max(std::abs(left), std::abs(right)) * 1e-6);
}

}  // namespace

RuntimeIdentity make_runtime_identity(
    const config::LoadedConfig& configuration,
    const std::uint64_t configuration_epoch) {
  if (configuration_epoch == 0U) {
    throw std::invalid_argument("configuration epoch must be nonzero");
  }
  RuntimeIdentity result;
  // Until a unit is provisioned, this stable engineering identity is derived
  // from the effective configuration. Factory provisioning replaces it with
  // the UUID stored in /var/lib/nuvo-depth/identity/device.uuid.
  result.device_uuid = uuid_from_hex(configuration.configuration_sha256, 0U);
  result.run_uuid = random_uuid();
  result.configuration_epoch = configuration_epoch;
  return result;
}

RuntimeIdentity make_runtime_identity(
    const config::LoadedConfig& configuration,
    const std::string_view device_uuid,
    const std::uint64_t configuration_epoch) {
  auto result = make_runtime_identity(configuration, configuration_epoch);
  result.device_uuid = parse_uuid(device_uuid);
  return result;
}

std::unique_ptr<NativeProcessingPipeline> make_native_pipeline(
    const config::EffectiveConfig& configuration,
    const PreparedStereoCalibration& calibration,
    const RuntimeIdentity& identity,
    const RuntimeFactoryOptions& options) {
  if (configuration.capture.width != calibration.image_view.grid.width ||
      configuration.capture.height != calibration.image_view.grid.height ||
      configuration.stereo.working_width !=
          calibration.matching_view.grid.width ||
      configuration.stereo.working_height !=
          calibration.matching_view.grid.height) {
    throw std::invalid_argument(
        "capture/matching geometry does not match prepared calibration");
  }
  if (calibration.identity.sha256 != calibration.image_view.identity.sha256 ||
      calibration.identity.sha256 != calibration.matching_view.identity.sha256 ||
      calibration.authority != calibration.image_view.authority ||
      calibration.authority != calibration.matching_view.authority) {
    throw std::invalid_argument(
        "prepared calibration views disagree with their identity or authority");
  }
  // The YAML describes the nominal rig and its provisional seed. A measured,
  // validated per-unit calibration supplies the metric geometry at runtime;
  // forcing it to reproduce the seed would discard the calibration's purpose.
  if (calibration.authority ==
          CalibrationAuthority::kProvisionalRecalibrationRequired &&
      (!near(configuration.rig.baseline_m, calibration.image_view.baseline_m) ||
       !near(configuration.stereo.baseline_m,
             calibration.image_view.baseline_m) ||
       !near(configuration.stereo.rectified_focal_length_px,
             calibration.image_view.left_projection[0]))) {
    throw std::invalid_argument(
        "runtime geometry disagrees with prepared calibration projections");
  }
  if (calibration.authority ==
          CalibrationAuthority::kProvisionalRecalibrationRequired &&
      !options.allow_provisional_depth_for_engineering) {
    throw std::invalid_argument(
        "provisional calibration requires the explicit engineering-depth opt-in");
  }

  RectificationBackendRegistry rectifiers;
  register_opencv_rectification_backend(rectifiers);
#if defined(DEPTHD_RUNTIME_WITH_EGL_GL)
  EglGlDmabufRectificationOptions egl_options;
  egl_options.allow_cpu_fallback =
      configuration.rectification.allow_fallback &&
      configuration.rectification.fallback_backend == "cpu_opencv";
  register_egl_gl_dmabuf_rectification_backend(rectifiers, egl_options);
#endif
  RectificationBackendConfig rectifier_configuration;
  rectifier_configuration.input_width = configuration.capture.width;
  rectifier_configuration.input_height = configuration.capture.height;
  rectifier_configuration.output_width = configuration.capture.width;
  rectifier_configuration.output_height = configuration.capture.height;
  const auto cpu_maps = options.cpu_rectification_maps ? options.cpu_rectification_maps :
      prepare_opencv_rectification_maps(calibration.maps);
  auto rectifier = configuration.rectification.backend == "cpu_opencv" ?
      make_opencv_rectification_backend(rectifier_configuration, cpu_maps) :
      rectifiers.create(configuration.rectification.backend,
          configuration.rectification.fallback_backend,
          configuration.rectification.allow_fallback, rectifier_configuration,
          calibration.maps);
  auto rgb_rectifier = rectifier->name() == kOpenCvRectificationBackendName ?
      std::unique_ptr<RectificationBackend>{} :
      make_opencv_rectification_backend(rectifier_configuration, cpu_maps);

  CpuSgbmConfig stereo_configuration;
  stereo_configuration.control_file = options.stereo_control_file;
  stereo_configuration.input_width = configuration.capture.width;
  stereo_configuration.input_height = configuration.capture.height;
  stereo_configuration.working_width = configuration.stereo.working_width;
  stereo_configuration.working_height = configuration.stereo.working_height;
  const auto disparity_scale =
      static_cast<double>(configuration.stereo.output_width) /
      static_cast<double>(configuration.stereo.working_width);
  stereo_configuration.working_min_disparity = static_cast<std::int32_t>(
      std::floor(static_cast<double>(configuration.stereo.min_disparity) /
                 disparity_scale));
  stereo_configuration.accepted_min_disparity_px = static_cast<float>(
      static_cast<double>(configuration.stereo.min_disparity) /
      disparity_scale);
  stereo_configuration.accepted_max_disparity_px = static_cast<float>(
      static_cast<double>(configuration.stereo.max_disparity) /
      disparity_scale);
  stereo_configuration.working_disparities =
      static_cast<std::int32_t>(configuration.stereo.working_disparities);
  stereo_configuration.block_size =
      static_cast<std::int32_t>(configuration.stereo.block_size);
  if (configuration.stereo.mode == "sgbm_3way") {
    stereo_configuration.sgbm_mode = "3way";
  } else if (configuration.stereo.mode == "sgbm_standard") {
    stereo_configuration.sgbm_mode = "standard";
  } else if (configuration.stereo.mode == "sgbm_hh4") {
    stereo_configuration.sgbm_mode = "hh4";
  } else if (configuration.stereo.mode == "sgbm_hh") {
    stereo_configuration.sgbm_mode = "hh";
  } else {
    throw std::invalid_argument("unsupported runtime stereo aggregation mode");
  }
  stereo_configuration.median_filter_size =
      static_cast<std::int32_t>(configuration.stereo.median_filter_size);
  stereo_configuration.uniqueness_ratio =
      static_cast<std::int32_t>(configuration.stereo.uniqueness_ratio);
  stereo_configuration.speckle_window_size =
      static_cast<std::int32_t>(configuration.stereo.speckle_window_size);
  stereo_configuration.speckle_range =
      static_cast<std::int32_t>(configuration.stereo.speckle_range);
  stereo_configuration.disparity_12_max_diff =
      configuration.stereo.disparity_12_max_diff;
  stereo_configuration.pre_filter_cap =
      static_cast<std::int32_t>(configuration.stereo.pre_filter_cap);
  stereo_configuration.opencv_threads =
      static_cast<std::int32_t>(configuration.stereo.opencv_threads);

  NativePipelineConfiguration pipeline_configuration;
  pipeline_configuration.image_only = options.image_only;
  pipeline_configuration.configuration_epochs = options.configuration_epochs;
  pipeline_configuration.frame_pool_capacity = bounded_frame_pool_capacity(
      calibration.image_view.grid, calibration.matching_view.grid,
      options.frame_pool_capacity, options.frame_pool_payload_budget_bytes, options.image_only);
  if (pipeline_configuration.frame_pool_capacity < 2U) {
    throw std::invalid_argument(
        "native publication requires at least two frame slots; increase the explicit payload budget or reduce geometry");
  }
  pipeline_configuration.frame_pool_payload_budget_bytes = options.frame_pool_payload_budget_bytes;
  pipeline_configuration.device_uuid = identity.device_uuid;
  pipeline_configuration.run_uuid = identity.run_uuid;
  pipeline_configuration.configuration_epoch = identity.configuration_epoch;
  pipeline_configuration.image_calibration = calibration.image_view;
  pipeline_configuration.matching_calibration = calibration.matching_view;
  pipeline_configuration.depth.minimum_depth_m = options.minimum_depth_m;
  pipeline_configuration.depth.maximum_depth_m = options.maximum_depth_m;
  pipeline_configuration.depth.allow_provisional_for_engineering =
      options.allow_provisional_depth_for_engineering;
  return std::make_unique<NativeProcessingPipeline>(
      pipeline_configuration, std::move(rectifier),
      options.image_only ? make_cpu_sgbm_metadata_backend(stereo_configuration) :
                           make_cpu_sgbm_native_backend(stereo_configuration),
      std::move(rgb_rectifier));
}

}  // namespace depthd
