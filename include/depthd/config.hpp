#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace depthd::config {

inline constexpr std::uint32_t kCurrentEffectiveSchemaVersion = 1;

enum class ConfigErrorKind {
  kValidation,
  kIo,
};

class ConfigError final : public std::runtime_error {
 public:
  ConfigError(ConfigErrorKind kind, std::string path, std::string message);

  [[nodiscard]] ConfigErrorKind kind() const noexcept;
  [[nodiscard]] const std::string& path() const noexcept;

 private:
  ConfigErrorKind kind_;
  std::string path_;
};

enum class CalibrationStatus {
  kProvisionalRecalibrationRequired,
  kFactoryValidated,
  kUserValidated,
};

// These structures mirror schema-v1 input deliberately. They are a migration
// boundary, not the long-term daemon or public SDK contract.
struct LegacyProductV1 {
  std::string platform;
  std::string role;
  std::string profile_id;
};

struct LegacyRigV1 {
  std::string sensor;
  std::uint32_t camera_count{};
  double baseline_m{};
  CalibrationStatus calibration_status{
      CalibrationStatus::kProvisionalRecalibrationRequired};
};

struct LegacyHardwareTriggerV1 {
  bool enabled{};
  std::string driver_mode;
  bool requires_always_on_overlay{};
  std::string gpiochip;
  std::uint32_t line{};
  std::string idle_level;
  std::string active_level;
  std::uint32_t pulse_width_us{};
  std::uint32_t startup_priming_pulses{};
  std::uint32_t pair_tolerance_us{};
};

struct LegacyAutoExposureV1 {
  bool enabled{};
  std::vector<std::uint32_t> metering_roi;
  std::uint32_t target_median{};
  std::vector<std::uint32_t> median_deadband;
  std::uint32_t dark_threshold{};
  std::uint32_t clipping_threshold{};
  double max_clipped_fraction{};
  std::uint32_t min_shutter_us{};
  std::uint32_t max_shutter_us{};
  double min_analogue_gain{};
  double max_analogue_gain{};
  std::uint32_t update_interval_frames{};
  std::uint32_t settle_frames_after_update{};
};

struct LegacyCaptureV1 {
  std::string backend;
  std::uint32_t left_camera_index{};
  std::uint32_t right_camera_index{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::string stream_profile;
  std::string main_pixel_format;
  std::string depth_plane;
  std::string left_color_stream;
  std::string left_color_pixel_format;
  std::string left_color_memory_order;
  bool right_color_stream_enabled{};
  std::string colour_space;
  std::uint32_t orientation_degrees{};
  double target_hz{};
  std::uint32_t buffer_count{};
  std::string handoff;
  bool zero_copy_camera_input{};
  LegacyHardwareTriggerV1 hardware_trigger;
  std::uint32_t exposure_time_us{};
  double analogue_gain{};
  std::vector<double> colour_gains;
  LegacyAutoExposureV1 auto_exposure;
};

struct LegacyRectificationV1 {
  std::string backend;
  std::string fallback_backend;
  bool allow_fallback{};
  std::string left_calibration;
  std::string right_calibration;
  std::string map_format;
  std::string depth_preprocessing;
  std::string color_rectification;
};

struct LegacyStereoV1 {
  std::string backend;
  double baseline_m{};
  double rectified_focal_length_px{};
  std::uint32_t output_width{};
  std::uint32_t output_height{};
  std::int32_t min_disparity{};
  std::int32_t max_disparity{};
  std::uint32_t working_width{};
  std::uint32_t working_height{};
  std::uint32_t working_disparities{};
  std::uint32_t block_size{};
  std::string mode;
  std::uint32_t uniqueness_ratio{};
  std::uint32_t speckle_window_size{};
  std::uint32_t speckle_range{};
  std::int32_t disparity_12_max_diff{};
  std::uint32_t pre_filter_cap{};
  std::uint32_t opencv_threads{};
  std::uint32_t median_filter_size{};
};

struct LegacyOutputsV1 {
  double disparity_hz{};
  bool metric_depth_enabled{};
  double compressed_image_hz{};
  std::uint32_t jpeg_quality{};
};

struct LegacyProfileV1 {
  std::uint32_t schema_version{};
  LegacyProductV1 product;
  LegacyRigV1 rig;
  LegacyCaptureV1 capture;
  LegacyRectificationV1 rectification;
  LegacyStereoV1 stereo;
  LegacyOutputsV1 outputs;
};

// EffectiveConfig contains the normalized settings used by native construction.
// Retired schema-v1 output keys are accepted only at the import boundary.
struct ProductConfig {
  std::string platform;
  std::string role;
  std::string profile_id;
};

struct RigConfig {
  std::string sensor;
  std::uint32_t camera_count{};
  double baseline_m{};
  CalibrationStatus calibration_status{
      CalibrationStatus::kProvisionalRecalibrationRequired};
};

struct HardwareTriggerConfig {
  bool enabled{};
  std::string driver_mode;
  bool requires_always_on_overlay{};
  std::string gpiochip;
  std::uint32_t line{};
  std::string idle_level;
  std::string active_level;
  std::uint32_t pulse_width_us{};
  std::uint32_t startup_priming_pulses{};
  std::uint32_t pair_tolerance_us{};
};

struct AutoExposureConfig {
  bool enabled{};
  std::vector<std::uint32_t> metering_roi;
  std::uint32_t target_median{};
  std::vector<std::uint32_t> median_deadband;
  std::uint32_t dark_threshold{};
  std::uint32_t clipping_threshold{};
  double max_clipped_fraction{};
  std::uint32_t min_shutter_us{};
  std::uint32_t max_shutter_us{};
  double min_analogue_gain{};
  double max_analogue_gain{};
  std::uint32_t update_interval_frames{};
  std::uint32_t settle_frames_after_update{};
};

struct CaptureConfig {
  std::string backend;
  std::uint32_t left_camera_index{};
  std::uint32_t right_camera_index{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::string stream_profile;
  std::string main_pixel_format;
  std::string depth_plane;
  std::string left_color_stream;
  std::string left_color_pixel_format;
  std::string left_color_memory_order;
  bool right_color_stream_enabled{};
  std::string colour_space;
  std::uint32_t orientation_degrees{};
  double target_hz{};
  std::uint32_t buffer_count{};
  std::string handoff;
  bool zero_copy_camera_input{};
  HardwareTriggerConfig hardware_trigger;
  std::uint32_t exposure_time_us{};
  double analogue_gain{};
  std::vector<double> colour_gains;
  AutoExposureConfig auto_exposure;
};

struct RectificationConfig {
  std::string backend;
  std::string fallback_backend;
  bool allow_fallback{};
  std::string left_calibration;
  std::string right_calibration;
  std::string map_format;
  std::string depth_preprocessing;
  std::string color_rectification;
};

struct StereoConfig {
  std::string backend;
  double baseline_m{};
  double rectified_focal_length_px{};
  std::uint32_t output_width{};
  std::uint32_t output_height{};
  std::int32_t min_disparity{};
  std::int32_t max_disparity{};
  std::uint32_t working_width{};
  std::uint32_t working_height{};
  std::uint32_t working_disparities{};
  std::uint32_t block_size{};
  std::string mode;
  std::uint32_t uniqueness_ratio{};
  std::uint32_t speckle_window_size{};
  std::uint32_t speckle_range{};
  std::int32_t disparity_12_max_diff{};
  std::uint32_t pre_filter_cap{};
  std::uint32_t opencv_threads{};
  std::uint32_t median_filter_size{};
};

struct OutputConfig {
  double disparity_hz{};
  bool metric_depth_enabled{};
  double compressed_image_hz{};
  std::uint32_t jpeg_quality{};
};

struct EffectiveConfig {
  std::uint32_t schema_version{kCurrentEffectiveSchemaVersion};
  std::uint32_t source_schema_version{};
  ProductConfig product;
  RigConfig rig;
  CaptureConfig capture;
  RectificationConfig rectification;
  StereoConfig stereo;
  OutputConfig outputs;
};

struct ConfigWarning {
  std::string path;
  std::string message;
};

struct LoadedConfig {
  EffectiveConfig effective;
  std::string configuration_sha256;
  std::vector<ConfigWarning> warnings;
};

[[nodiscard]] LoadedConfig load(const std::filesystem::path& path);
[[nodiscard]] LegacyProfileV1 load_legacy_v1(
    const std::filesystem::path& path);
[[nodiscard]] EffectiveConfig normalize(const LegacyProfileV1& legacy);
void validate(const EffectiveConfig& config);

// The canonical form excludes warnings and its own digest. The dump wraps this
// object with provenance and warnings while keeping stdout valid JSON.
[[nodiscard]] std::string canonical_json(const EffectiveConfig& config);
[[nodiscard]] std::string dump_json(const LoadedConfig& loaded);
[[nodiscard]] std::string sha256_hex(std::string_view bytes);
[[nodiscard]] std::string_view calibration_status_name(
    CalibrationStatus status) noexcept;

}  // namespace depthd::config
