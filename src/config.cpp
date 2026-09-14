#include "depthd/config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace depthd::config {
namespace {

constexpr std::uint32_t kMaxImageDimension = 16'384;
constexpr std::uint32_t kMaxBufferCount = 64;

[[noreturn]] void validation_error(const std::string& path,
                                   const std::string& message) {
  throw ConfigError(ConfigErrorKind::kValidation, path, message);
}

std::string mark_suffix(const YAML::Mark& mark) {
  if (mark.line < 0 || mark.column < 0) {
    return {};
  }
  return " at line " + std::to_string(mark.line + 1) + ", column " +
         std::to_string(mark.column + 1);
}

void check_mapping(const YAML::Node& node, const std::string& path,
                   const std::initializer_list<std::string_view> keys) {
  if (!node || !node.IsMap()) {
    validation_error(path, "expected a mapping");
  }

  std::set<std::string, std::less<>> allowed;
  for (const auto key : keys) {
    allowed.emplace(key);
  }

  std::set<std::string, std::less<>> seen;
  for (const auto& entry : node) {
    if (!entry.first.IsScalar()) {
      validation_error(path, "mapping keys must be strings");
    }
    const auto key = entry.first.Scalar();
    const auto key_path = path.empty() ? key : path + "." + key;
    if (!seen.emplace(key).second) {
      validation_error(key_path, "duplicate key");
    }
    if (!allowed.contains(key)) {
      validation_error(key_path, "unknown key");
    }
  }

  for (const auto& key : allowed) {
    if (!seen.contains(key)) {
      validation_error(path.empty() ? key : path + "." + key,
                       "missing required key");
    }
  }
}

YAML::Node child(const YAML::Node& parent, const std::string_view key) {
  return parent[std::string(key)];
}

std::string read_string(const YAML::Node& node, const std::string& path) {
  if (!node || !node.IsScalar()) {
    validation_error(path, "expected a string");
  }
  try {
    auto value = node.as<std::string>();
    if (value.empty()) {
      validation_error(path, "must not be empty");
    }
    return value;
  } catch (const YAML::Exception& error) {
    validation_error(path, "expected a string" + mark_suffix(error.mark));
  }
}

bool read_bool(const YAML::Node& node, const std::string& path) {
  if (!node || !node.IsScalar()) {
    validation_error(path, "expected a boolean");
  }
  try {
    return node.as<bool>();
  } catch (const YAML::Exception& error) {
    validation_error(path, "expected a boolean" + mark_suffix(error.mark));
  }
}

std::uint32_t read_u32(const YAML::Node& node, const std::string& path) {
  if (!node || !node.IsScalar()) {
    validation_error(path, "expected an unsigned integer");
  }
  try {
    const auto value = node.as<std::uint64_t>();
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      validation_error(path, "unsigned integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(value);
  } catch (const YAML::Exception& error) {
    validation_error(path,
                     "expected an unsigned integer" + mark_suffix(error.mark));
  }
}

std::int32_t read_i32(const YAML::Node& node, const std::string& path) {
  if (!node || !node.IsScalar()) {
    validation_error(path, "expected an integer");
  }
  try {
    const auto value = node.as<std::int64_t>();
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
      validation_error(path, "integer exceeds int32 range");
    }
    return static_cast<std::int32_t>(value);
  } catch (const YAML::Exception& error) {
    validation_error(path, "expected an integer" + mark_suffix(error.mark));
  }
}

double read_double(const YAML::Node& node, const std::string& path) {
  if (!node || !node.IsScalar()) {
    validation_error(path, "expected a finite number");
  }
  try {
    const auto value = node.as<double>();
    if (!std::isfinite(value)) {
      validation_error(path, "must be finite; NaN and infinity are invalid");
    }
    return value;
  } catch (const YAML::Exception& error) {
    validation_error(path, "expected a finite number" + mark_suffix(error.mark));
  }
}

std::vector<std::uint32_t> read_u32_sequence(const YAML::Node& node,
                                             const std::string& path,
                                             const std::size_t size) {
  if (!node || !node.IsSequence()) {
    validation_error(path, "expected a sequence");
  }
  if (node.size() != size) {
    validation_error(path, "expected exactly " + std::to_string(size) +
                               " elements");
  }
  std::vector<std::uint32_t> result;
  result.reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    result.push_back(
        read_u32(node[index], path + "[" + std::to_string(index) + "]"));
  }
  return result;
}

std::vector<double> read_double_sequence(const YAML::Node& node,
                                         const std::string& path,
                                         const std::size_t size) {
  if (!node || !node.IsSequence()) {
    validation_error(path, "expected a sequence");
  }
  if (node.size() != size) {
    validation_error(path, "expected exactly " + std::to_string(size) +
                               " elements");
  }
  std::vector<double> result;
  result.reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    result.push_back(
        read_double(node[index], path + "[" + std::to_string(index) + "]"));
  }
  return result;
}

CalibrationStatus read_calibration_status(const YAML::Node& node,
                                          const std::string& path) {
  const auto value = read_string(node, path);
  if (value == "provisional_recalibration_required") {
    return CalibrationStatus::kProvisionalRecalibrationRequired;
  }
  if (value == "factory_validated") {
    return CalibrationStatus::kFactoryValidated;
  }
  if (value == "user_validated") {
    return CalibrationStatus::kUserValidated;
  }
  validation_error(path, "unknown calibration status '" + value + "'");
}

LegacyProductV1 parse_product(const YAML::Node& node) {
  constexpr auto path = "product";
  check_mapping(node, path, {"platform", "role", "profile_id"});
  return {read_string(child(node, "platform"), "product.platform"),
          read_string(child(node, "role"), "product.role"),
          read_string(child(node, "profile_id"), "product.profile_id")};
}

LegacyRigV1 parse_rig(const YAML::Node& node) {
  constexpr auto path = "rig";
  check_mapping(node, path,
                {"sensor", "camera_count", "baseline_m",
                 "calibration_status"});
  LegacyRigV1 result;
  result.sensor = read_string(child(node, "sensor"), "rig.sensor");
  result.camera_count =
      read_u32(child(node, "camera_count"), "rig.camera_count");
  result.baseline_m = read_double(child(node, "baseline_m"), "rig.baseline_m");
  result.calibration_status = read_calibration_status(
      child(node, "calibration_status"), "rig.calibration_status");
  return result;
}

LegacyHardwareTriggerV1 parse_hardware_trigger(const YAML::Node& node) {
  constexpr auto path = "capture.hardware_trigger";
  check_mapping(node, path,
                {"enabled", "driver_mode", "requires_always_on_overlay",
                 "gpiochip", "line", "idle_level", "active_level",
                 "pulse_width_us", "startup_priming_pulses",
                 "pair_tolerance_us"});
  LegacyHardwareTriggerV1 result;
  result.enabled = read_bool(child(node, "enabled"), path + std::string(".enabled"));
  result.driver_mode =
      read_string(child(node, "driver_mode"), path + std::string(".driver_mode"));
  result.requires_always_on_overlay = read_bool(
      child(node, "requires_always_on_overlay"),
      path + std::string(".requires_always_on_overlay"));
  result.gpiochip =
      read_string(child(node, "gpiochip"), path + std::string(".gpiochip"));
  result.line = read_u32(child(node, "line"), path + std::string(".line"));
  result.idle_level =
      read_string(child(node, "idle_level"), path + std::string(".idle_level"));
  result.active_level = read_string(child(node, "active_level"),
                                    path + std::string(".active_level"));
  result.pulse_width_us = read_u32(child(node, "pulse_width_us"),
                                   path + std::string(".pulse_width_us"));
  result.startup_priming_pulses = read_u32(
      child(node, "startup_priming_pulses"),
      path + std::string(".startup_priming_pulses"));
  result.pair_tolerance_us = read_u32(
      child(node, "pair_tolerance_us"),
      path + std::string(".pair_tolerance_us"));
  return result;
}

LegacyAutoExposureV1 parse_auto_exposure(const YAML::Node& node) {
  constexpr auto path = "capture.auto_exposure";
  check_mapping(node, path,
                {"enabled", "metering_roi", "target_median",
                 "median_deadband", "dark_threshold", "clipping_threshold",
                 "max_clipped_fraction", "min_shutter_us", "max_shutter_us",
                 "min_analogue_gain", "max_analogue_gain",
                 "update_interval_frames", "settle_frames_after_update"});
  LegacyAutoExposureV1 result;
  result.enabled = read_bool(child(node, "enabled"), path + std::string(".enabled"));
  result.metering_roi = read_u32_sequence(
      child(node, "metering_roi"), path + std::string(".metering_roi"), 4);
  result.target_median = read_u32(child(node, "target_median"),
                                  path + std::string(".target_median"));
  result.median_deadband = read_u32_sequence(
      child(node, "median_deadband"), path + std::string(".median_deadband"), 2);
  result.dark_threshold = read_u32(child(node, "dark_threshold"),
                                   path + std::string(".dark_threshold"));
  result.clipping_threshold = read_u32(
      child(node, "clipping_threshold"),
      path + std::string(".clipping_threshold"));
  result.max_clipped_fraction = read_double(
      child(node, "max_clipped_fraction"),
      path + std::string(".max_clipped_fraction"));
  result.min_shutter_us = read_u32(child(node, "min_shutter_us"),
                                   path + std::string(".min_shutter_us"));
  result.max_shutter_us = read_u32(child(node, "max_shutter_us"),
                                   path + std::string(".max_shutter_us"));
  result.min_analogue_gain = read_double(
      child(node, "min_analogue_gain"),
      path + std::string(".min_analogue_gain"));
  result.max_analogue_gain = read_double(
      child(node, "max_analogue_gain"),
      path + std::string(".max_analogue_gain"));
  result.update_interval_frames = read_u32(
      child(node, "update_interval_frames"),
      path + std::string(".update_interval_frames"));
  result.settle_frames_after_update = read_u32(
      child(node, "settle_frames_after_update"),
      path + std::string(".settle_frames_after_update"));
  return result;
}

LegacyCaptureV1 parse_capture(const YAML::Node& node) {
  constexpr auto path = "capture";
  check_mapping(
      node, path,
      {"backend", "left_camera_index", "right_camera_index", "width",
       "height", "stream_profile", "main_pixel_format", "depth_plane",
       "left_color_stream", "left_color_pixel_format",
       "left_color_memory_order", "right_color_stream_enabled",
       "colour_space", "orientation_degrees", "target_hz", "buffer_count",
       "handoff", "zero_copy_camera_input", "hardware_trigger",
       "exposure_time_us", "analogue_gain", "colour_gains",
       "auto_exposure"});
  LegacyCaptureV1 result;
  result.backend = read_string(child(node, "backend"), "capture.backend");
  result.left_camera_index =
      read_u32(child(node, "left_camera_index"), "capture.left_camera_index");
  result.right_camera_index = read_u32(child(node, "right_camera_index"),
                                       "capture.right_camera_index");
  result.width = read_u32(child(node, "width"), "capture.width");
  result.height = read_u32(child(node, "height"), "capture.height");
  result.stream_profile =
      read_string(child(node, "stream_profile"), "capture.stream_profile");
  result.main_pixel_format = read_string(child(node, "main_pixel_format"),
                                         "capture.main_pixel_format");
  result.depth_plane =
      read_string(child(node, "depth_plane"), "capture.depth_plane");
  result.left_color_stream = read_string(child(node, "left_color_stream"),
                                         "capture.left_color_stream");
  result.left_color_pixel_format = read_string(
      child(node, "left_color_pixel_format"),
      "capture.left_color_pixel_format");
  result.left_color_memory_order = read_string(
      child(node, "left_color_memory_order"),
      "capture.left_color_memory_order");
  result.right_color_stream_enabled = read_bool(
      child(node, "right_color_stream_enabled"),
      "capture.right_color_stream_enabled");
  result.colour_space =
      read_string(child(node, "colour_space"), "capture.colour_space");
  result.orientation_degrees = read_u32(child(node, "orientation_degrees"),
                                        "capture.orientation_degrees");
  result.target_hz =
      read_double(child(node, "target_hz"), "capture.target_hz");
  result.buffer_count =
      read_u32(child(node, "buffer_count"), "capture.buffer_count");
  result.handoff = read_string(child(node, "handoff"), "capture.handoff");
  result.zero_copy_camera_input = read_bool(
      child(node, "zero_copy_camera_input"),
      "capture.zero_copy_camera_input");
  result.hardware_trigger =
      parse_hardware_trigger(child(node, "hardware_trigger"));
  result.exposure_time_us = read_u32(child(node, "exposure_time_us"),
                                     "capture.exposure_time_us");
  result.analogue_gain =
      read_double(child(node, "analogue_gain"), "capture.analogue_gain");
  result.colour_gains = read_double_sequence(
      child(node, "colour_gains"), "capture.colour_gains", 2);
  result.auto_exposure = parse_auto_exposure(child(node, "auto_exposure"));
  return result;
}

LegacyRectificationV1 parse_rectification(const YAML::Node& node) {
  constexpr auto path = "rectification";
  check_mapping(node, path,
                {"backend", "fallback_backend", "allow_fallback",
                 "left_calibration", "right_calibration", "map_format",
                 "depth_preprocessing", "color_rectification"});
  LegacyRectificationV1 result;
  result.backend =
      read_string(child(node, "backend"), "rectification.backend");
  result.fallback_backend = read_string(child(node, "fallback_backend"),
                                        "rectification.fallback_backend");
  result.allow_fallback = read_bool(child(node, "allow_fallback"),
                                    "rectification.allow_fallback");
  result.left_calibration = read_string(child(node, "left_calibration"),
                                        "rectification.left_calibration");
  result.right_calibration = read_string(child(node, "right_calibration"),
                                         "rectification.right_calibration");
  result.map_format =
      read_string(child(node, "map_format"), "rectification.map_format");
  result.depth_preprocessing = read_string(
      child(node, "depth_preprocessing"),
      "rectification.depth_preprocessing");
  result.color_rectification = read_string(
      child(node, "color_rectification"),
      "rectification.color_rectification");
  return result;
}

LegacyStereoV1 parse_stereo(const YAML::Node& node) {
  constexpr auto path = "stereo";
  check_mapping(node, path,
                {"backend", "baseline_m", "rectified_focal_length_px",
                 "output_width", "output_height", "min_disparity",
                 "max_disparity", "working_width", "working_height",
                 "working_disparities", "block_size", "mode",
                 "uniqueness_ratio", "speckle_window_size", "speckle_range",
                 "disparity_12_max_diff", "pre_filter_cap",
                 "opencv_threads"});
  LegacyStereoV1 result;
  result.backend = read_string(child(node, "backend"), "stereo.backend");
  result.baseline_m =
      read_double(child(node, "baseline_m"), "stereo.baseline_m");
  result.rectified_focal_length_px = read_double(
      child(node, "rectified_focal_length_px"),
      "stereo.rectified_focal_length_px");
  result.output_width =
      read_u32(child(node, "output_width"), "stereo.output_width");
  result.output_height =
      read_u32(child(node, "output_height"), "stereo.output_height");
  result.min_disparity =
      read_i32(child(node, "min_disparity"), "stereo.min_disparity");
  result.max_disparity =
      read_i32(child(node, "max_disparity"), "stereo.max_disparity");
  result.working_width =
      read_u32(child(node, "working_width"), "stereo.working_width");
  result.working_height =
      read_u32(child(node, "working_height"), "stereo.working_height");
  result.working_disparities = read_u32(
      child(node, "working_disparities"), "stereo.working_disparities");
  result.block_size =
      read_u32(child(node, "block_size"), "stereo.block_size");
  result.mode = read_string(child(node, "mode"), "stereo.mode");
  result.uniqueness_ratio = read_u32(child(node, "uniqueness_ratio"),
                                     "stereo.uniqueness_ratio");
  result.speckle_window_size = read_u32(child(node, "speckle_window_size"),
                                        "stereo.speckle_window_size");
  result.speckle_range =
      read_u32(child(node, "speckle_range"), "stereo.speckle_range");
  result.disparity_12_max_diff = read_i32(
      child(node, "disparity_12_max_diff"),
      "stereo.disparity_12_max_diff");
  result.pre_filter_cap =
      read_u32(child(node, "pre_filter_cap"), "stereo.pre_filter_cap");
  result.opencv_threads =
      read_u32(child(node, "opencv_threads"), "stereo.opencv_threads");
  return result;
}

LegacyOutputsV1 parse_outputs(const YAML::Node& node) {
  constexpr auto path = "outputs";
  check_mapping(node, path,
                {"disparity_hz", "disparity_topic", "metric_depth_enabled",
                 "compressed_image_hz", "compressed_image_topic",
                 "jpeg_quality", "left_camera_info_topic",
                 "diagnostics_topic", "obstacle_cloud_enabled",
                 "obstacle_cloud_topic", "result_ring_path",
                 "result_ring_slots", "result_ring_left_mono_enabled"});
  LegacyOutputsV1 result;
  result.disparity_hz =
      read_double(child(node, "disparity_hz"), "outputs.disparity_hz");
  result.disparity_topic =
      read_string(child(node, "disparity_topic"), "outputs.disparity_topic");
  result.metric_depth_enabled = read_bool(
      child(node, "metric_depth_enabled"), "outputs.metric_depth_enabled");
  result.compressed_image_hz = read_double(
      child(node, "compressed_image_hz"), "outputs.compressed_image_hz");
  result.compressed_image_topic = read_string(
      child(node, "compressed_image_topic"),
      "outputs.compressed_image_topic");
  result.jpeg_quality =
      read_u32(child(node, "jpeg_quality"), "outputs.jpeg_quality");
  result.left_camera_info_topic = read_string(
      child(node, "left_camera_info_topic"),
      "outputs.left_camera_info_topic");
  result.diagnostics_topic = read_string(child(node, "diagnostics_topic"),
                                         "outputs.diagnostics_topic");
  result.obstacle_cloud_enabled = read_bool(
      child(node, "obstacle_cloud_enabled"),
      "outputs.obstacle_cloud_enabled");
  result.obstacle_cloud_topic = read_string(
      child(node, "obstacle_cloud_topic"), "outputs.obstacle_cloud_topic");
  result.result_ring_path = read_string(child(node, "result_ring_path"),
                                        "outputs.result_ring_path");
  result.result_ring_slots = read_u32(child(node, "result_ring_slots"),
                                      "outputs.result_ring_slots");
  result.result_ring_left_mono_enabled = read_bool(
      child(node, "result_ring_left_mono_enabled"),
      "outputs.result_ring_left_mono_enabled");
  return result;
}

void require_equal(const std::string& value, const std::string_view expected,
                   const std::string& path) {
  if (value != expected) {
    validation_error(path, "unsupported value '" + value +
                               "'; schema v1 requires '" +
                               std::string(expected) + "'");
  }
}

void require_one_of(const std::string& value,
                    const std::initializer_list<std::string_view> allowed,
                    const std::string& path) {
  const auto found = std::any_of(
      allowed.begin(), allowed.end(),
      [&](const std::string_view candidate) { return value == candidate; });
  if (!found) {
    validation_error(path, "unsupported value '" + value + "'");
  }
}

void require_positive(const double value, const std::string& path) {
  if (!std::isfinite(value) || value <= 0.0) {
    validation_error(path, "must be finite and greater than zero");
  }
}

std::uint64_t checked_multiply(const std::uint64_t left,
                               const std::uint64_t right,
                               const std::string& path) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    validation_error(path, "computed frame storage overflows uint64");
  }
  return left * right;
}

void validate_path(const std::string& value, const std::string& path) {
  // The file describes a Linux target even when checked on Windows, so use
  // POSIX path rules rather than host std::filesystem absolute-path rules.
  if (value.empty() || value.front() != '/') {
    validation_error(path, "must be an absolute POSIX runtime path");
  }
  if (value == "/.." || value.starts_with("../") ||
      value.find("/../") != std::string::npos || value.ends_with("/..")) {
    validation_error(path, "must not contain '..' path components");
  }
}

void validate_topic(const std::string& value, const std::string& path) {
  if (value.empty() || value.front() != '/' || value.find(' ') != std::string::npos) {
    validation_error(path, "must be an absolute ROS topic without spaces");
  }
}

void append_escaped(std::string& output, const std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (character < 0x20U) {
          output += "\\u00";
          output.push_back(hex[(character >> 4U) & 0x0fU]);
          output.push_back(hex[character & 0x0fU]);
        } else {
          output.push_back(static_cast<char>(character));
        }
    }
  }
  output.push_back('"');
}

void append_key(std::string& output, bool& first, const std::string_view key) {
  if (!first) {
    output.push_back(',');
  }
  first = false;
  append_escaped(output, key);
  output.push_back(':');
}

void append_string_field(std::string& output, bool& first,
                         const std::string_view key,
                         const std::string_view value) {
  append_key(output, first, key);
  append_escaped(output, value);
}

void append_bool_field(std::string& output, bool& first,
                       const std::string_view key, const bool value) {
  append_key(output, first, key);
  output += value ? "true" : "false";
}

template <typename Integer>
void append_integer_field(std::string& output, bool& first,
                          const std::string_view key, const Integer value) {
  append_key(output, first, key);
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                    value);
  if (result.ec != std::errc{}) {
    throw std::runtime_error("failed to serialize integer configuration value");
  }
  output.append(buffer.data(), result.ptr);
}

void append_double_value(std::string& output, const double value) {
  std::array<char, 64> buffer{};
  const auto result = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(), value,
      std::chars_format::general, std::numeric_limits<double>::max_digits10);
  if (result.ec != std::errc{}) {
    throw std::runtime_error("failed to serialize floating configuration value");
  }
  output.append(buffer.data(), result.ptr);
}

void append_double_field(std::string& output, bool& first,
                         const std::string_view key, const double value) {
  append_key(output, first, key);
  append_double_value(output, value);
}

void append_u32_array_field(std::string& output, bool& first,
                            const std::string_view key,
                            const std::vector<std::uint32_t>& values) {
  append_key(output, first, key);
  output.push_back('[');
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output.push_back(',');
    }
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), values[index]);
    if (result.ec != std::errc{}) {
      throw std::runtime_error("failed to serialize integer array value");
    }
    output.append(buffer.data(), result.ptr);
  }
  output.push_back(']');
}

void append_double_array_field(std::string& output, bool& first,
                               const std::string_view key,
                               const std::vector<double>& values) {
  append_key(output, first, key);
  output.push_back('[');
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output.push_back(',');
    }
    append_double_value(output, values[index]);
  }
  output.push_back(']');
}

template <typename Callback>
void append_object_field(std::string& output, bool& first,
                         const std::string_view key, Callback callback) {
  append_key(output, first, key);
  output.push_back('{');
  bool nested_first = true;
  callback(nested_first);
  output.push_back('}');
}

std::string serialize_effective(const EffectiveConfig& config) {
  std::string output;
  output.reserve(4096);
  output.push_back('{');
  bool root = true;
  append_integer_field(output, root, "schema_version", config.schema_version);
  append_integer_field(output, root, "source_schema_version",
                       config.source_schema_version);

  append_object_field(output, root, "product", [&](bool& first) {
    append_string_field(output, first, "platform", config.product.platform);
    append_string_field(output, first, "role", config.product.role);
    append_string_field(output, first, "profile_id", config.product.profile_id);
  });
  append_object_field(output, root, "rig", [&](bool& first) {
    append_string_field(output, first, "sensor", config.rig.sensor);
    append_integer_field(output, first, "camera_count", config.rig.camera_count);
    append_double_field(output, first, "baseline_m", config.rig.baseline_m);
    append_string_field(output, first, "calibration_status",
                        calibration_status_name(config.rig.calibration_status));
  });
  append_object_field(output, root, "capture", [&](bool& first) {
    const auto& capture = config.capture;
    append_string_field(output, first, "backend", capture.backend);
    append_integer_field(output, first, "left_camera_index",
                         capture.left_camera_index);
    append_integer_field(output, first, "right_camera_index",
                         capture.right_camera_index);
    append_integer_field(output, first, "width", capture.width);
    append_integer_field(output, first, "height", capture.height);
    append_string_field(output, first, "stream_profile", capture.stream_profile);
    append_string_field(output, first, "main_pixel_format",
                        capture.main_pixel_format);
    append_string_field(output, first, "depth_plane", capture.depth_plane);
    append_string_field(output, first, "left_color_stream",
                        capture.left_color_stream);
    append_string_field(output, first, "left_color_pixel_format",
                        capture.left_color_pixel_format);
    append_string_field(output, first, "left_color_memory_order",
                        capture.left_color_memory_order);
    append_bool_field(output, first, "right_color_stream_enabled",
                      capture.right_color_stream_enabled);
    append_string_field(output, first, "colour_space", capture.colour_space);
    append_integer_field(output, first, "orientation_degrees",
                         capture.orientation_degrees);
    append_double_field(output, first, "target_hz", capture.target_hz);
    append_integer_field(output, first, "buffer_count", capture.buffer_count);
    append_string_field(output, first, "handoff", capture.handoff);
    append_bool_field(output, first, "zero_copy_camera_input",
                      capture.zero_copy_camera_input);
    append_object_field(output, first, "hardware_trigger", [&](bool& nested) {
      const auto& trigger = capture.hardware_trigger;
      append_bool_field(output, nested, "enabled", trigger.enabled);
      append_string_field(output, nested, "driver_mode", trigger.driver_mode);
      append_bool_field(output, nested, "requires_always_on_overlay",
                        trigger.requires_always_on_overlay);
      append_string_field(output, nested, "gpiochip", trigger.gpiochip);
      append_integer_field(output, nested, "line", trigger.line);
      append_string_field(output, nested, "idle_level", trigger.idle_level);
      append_string_field(output, nested, "active_level", trigger.active_level);
      append_integer_field(output, nested, "pulse_width_us",
                           trigger.pulse_width_us);
      append_integer_field(output, nested, "startup_priming_pulses",
                           trigger.startup_priming_pulses);
      append_integer_field(output, nested, "pair_tolerance_us",
                           trigger.pair_tolerance_us);
    });
    append_integer_field(output, first, "exposure_time_us",
                         capture.exposure_time_us);
    append_double_field(output, first, "analogue_gain", capture.analogue_gain);
    append_double_array_field(output, first, "colour_gains",
                              capture.colour_gains);
    append_object_field(output, first, "auto_exposure", [&](bool& nested) {
      const auto& exposure = capture.auto_exposure;
      append_bool_field(output, nested, "enabled", exposure.enabled);
      append_u32_array_field(output, nested, "metering_roi",
                             exposure.metering_roi);
      append_integer_field(output, nested, "target_median",
                           exposure.target_median);
      append_u32_array_field(output, nested, "median_deadband",
                             exposure.median_deadband);
      append_integer_field(output, nested, "dark_threshold",
                           exposure.dark_threshold);
      append_integer_field(output, nested, "clipping_threshold",
                           exposure.clipping_threshold);
      append_double_field(output, nested, "max_clipped_fraction",
                          exposure.max_clipped_fraction);
      append_integer_field(output, nested, "min_shutter_us",
                           exposure.min_shutter_us);
      append_integer_field(output, nested, "max_shutter_us",
                           exposure.max_shutter_us);
      append_double_field(output, nested, "min_analogue_gain",
                          exposure.min_analogue_gain);
      append_double_field(output, nested, "max_analogue_gain",
                          exposure.max_analogue_gain);
      append_integer_field(output, nested, "update_interval_frames",
                           exposure.update_interval_frames);
      append_integer_field(output, nested, "settle_frames_after_update",
                           exposure.settle_frames_after_update);
    });
  });
  append_object_field(output, root, "rectification", [&](bool& first) {
    const auto& rectification = config.rectification;
    append_string_field(output, first, "backend", rectification.backend);
    append_string_field(output, first, "fallback_backend",
                        rectification.fallback_backend);
    append_bool_field(output, first, "allow_fallback",
                      rectification.allow_fallback);
    append_string_field(output, first, "left_calibration",
                        rectification.left_calibration);
    append_string_field(output, first, "right_calibration",
                        rectification.right_calibration);
    append_string_field(output, first, "map_format", rectification.map_format);
    append_string_field(output, first, "depth_preprocessing",
                        rectification.depth_preprocessing);
    append_string_field(output, first, "color_rectification",
                        rectification.color_rectification);
  });
  append_object_field(output, root, "stereo", [&](bool& first) {
    const auto& stereo = config.stereo;
    append_string_field(output, first, "backend", stereo.backend);
    append_double_field(output, first, "baseline_m", stereo.baseline_m);
    append_double_field(output, first, "rectified_focal_length_px",
                        stereo.rectified_focal_length_px);
    append_integer_field(output, first, "output_width", stereo.output_width);
    append_integer_field(output, first, "output_height", stereo.output_height);
    append_integer_field(output, first, "min_disparity", stereo.min_disparity);
    append_integer_field(output, first, "max_disparity", stereo.max_disparity);
    append_integer_field(output, first, "working_width", stereo.working_width);
    append_integer_field(output, first, "working_height",
                         stereo.working_height);
    append_integer_field(output, first, "working_disparities",
                         stereo.working_disparities);
    append_integer_field(output, first, "block_size", stereo.block_size);
    append_string_field(output, first, "mode", stereo.mode);
    append_integer_field(output, first, "uniqueness_ratio",
                         stereo.uniqueness_ratio);
    append_integer_field(output, first, "speckle_window_size",
                         stereo.speckle_window_size);
    append_integer_field(output, first, "speckle_range", stereo.speckle_range);
    append_integer_field(output, first, "disparity_12_max_diff",
                         stereo.disparity_12_max_diff);
    append_integer_field(output, first, "pre_filter_cap",
                         stereo.pre_filter_cap);
    append_integer_field(output, first, "opencv_threads",
                         stereo.opencv_threads);
  });
  append_object_field(output, root, "outputs", [&](bool& first) {
    const auto& outputs = config.outputs;
    append_double_field(output, first, "disparity_hz", outputs.disparity_hz);
    append_bool_field(output, first, "metric_depth_enabled",
                      outputs.metric_depth_enabled);
    append_double_field(output, first, "compressed_image_hz",
                        outputs.compressed_image_hz);
    append_integer_field(output, first, "jpeg_quality", outputs.jpeg_quality);
    append_bool_field(output, first, "obstacle_cloud_enabled",
                      outputs.obstacle_cloud_enabled);
    append_string_field(output, first, "result_ring_path",
                        outputs.result_ring_path);
    append_integer_field(output, first, "result_ring_slots",
                         outputs.result_ring_slots);
    append_bool_field(output, first, "result_ring_left_mono_enabled",
                      outputs.result_ring_left_mono_enabled);
  });
  append_object_field(output, root, "legacy_ros_adapter", [&](bool& first) {
    append_string_field(output, first, "disparity_topic",
                        config.legacy_ros.disparity_topic);
    append_string_field(output, first, "compressed_image_topic",
                        config.legacy_ros.compressed_image_topic);
    append_string_field(output, first, "left_camera_info_topic",
                        config.legacy_ros.left_camera_info_topic);
    append_string_field(output, first, "diagnostics_topic",
                        config.legacy_ros.diagnostics_topic);
    append_string_field(output, first, "obstacle_cloud_topic",
                        config.legacy_ros.obstacle_cloud_topic);
  });
  output.push_back('}');
  return output;
}

class Sha256 final {
 public:
  void update(const std::string_view bytes) {
    total_bytes_ += bytes.size();
    for (const unsigned char byte : bytes) {
      buffer_[buffer_size_++] = byte;
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_.data());
        buffer_size_ = 0;
      }
    }
  }

  std::array<std::uint8_t, 32> finish() {
    const auto original_bits = static_cast<std::uint64_t>(total_bytes_) * 8U;
    buffer_[buffer_size_++] = 0x80U;
    if (buffer_size_ > 56U) {
      std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                buffer_.end(), 0U);
      transform(buffer_.data());
      buffer_size_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
              buffer_.begin() + 56, 0U);
    for (std::size_t index = 0; index < 8; ++index) {
      buffer_[63U - index] =
          static_cast<std::uint8_t>(original_bits >> (index * 8U));
    }
    transform(buffer_.data());

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < state_.size(); ++index) {
      digest[index * 4U] = static_cast<std::uint8_t>(state_[index] >> 24U);
      digest[index * 4U + 1U] =
          static_cast<std::uint8_t>(state_[index] >> 16U);
      digest[index * 4U + 2U] =
          static_cast<std::uint8_t>(state_[index] >> 8U);
      digest[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index]);
    }
    return digest;
  }

 private:
  static constexpr std::array<std::uint32_t, 64> kRoundConstants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  void transform(const std::uint8_t* block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const auto offset = index * 4U;
      words[index] = static_cast<std::uint32_t>(block[offset]) << 24U |
                     static_cast<std::uint32_t>(block[offset + 1U]) << 16U |
                     static_cast<std::uint32_t>(block[offset + 2U]) << 8U |
                     static_cast<std::uint32_t>(block[offset + 3U]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto s0 = std::rotr(words[index - 15U], 7) ^
                      std::rotr(words[index - 15U], 18) ^
                      (words[index - 15U] >> 3U);
      const auto s1 = std::rotr(words[index - 2U], 17) ^
                      std::rotr(words[index - 2U], 19) ^
                      (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];

    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto temporary1 =
          h + sum1 + choose + kRoundConstants[index] + words[index];
      const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_size_{};
  std::size_t total_bytes_{};
};

}  // namespace

ConfigError::ConfigError(const ConfigErrorKind kind, std::string path,
                         std::string message)
    : std::runtime_error(path.empty() ? std::move(message)
                                      : path + ": " + message),
      kind_(kind),
      path_(std::move(path)) {}

ConfigErrorKind ConfigError::kind() const noexcept { return kind_; }

const std::string& ConfigError::path() const noexcept { return path_; }

std::string_view calibration_status_name(const CalibrationStatus status) noexcept {
  switch (status) {
    case CalibrationStatus::kProvisionalRecalibrationRequired:
      return "provisional_recalibration_required";
    case CalibrationStatus::kFactoryValidated:
      return "factory_validated";
    case CalibrationStatus::kUserValidated:
      return "user_validated";
  }
  return "unknown";
}

LegacyProfileV1 load_legacy_v1(const std::filesystem::path& path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const YAML::BadFile& error) {
    throw ConfigError(ConfigErrorKind::kIo, path.string(), error.what());
  } catch (const YAML::Exception& error) {
    throw ConfigError(ConfigErrorKind::kValidation, path.string(),
                      std::string(error.what()) + mark_suffix(error.mark));
  }

  check_mapping(root, {},
                {"schema_version", "product", "rig", "capture",
                 "rectification", "stereo", "outputs"});
  const auto schema_version =
      read_u32(child(root, "schema_version"), "schema_version");
  if (schema_version != 1U) {
    validation_error("schema_version",
                     "unsupported schema version " +
                         std::to_string(schema_version) + "; supported: 1");
  }

  LegacyProfileV1 result;
  result.schema_version = schema_version;
  result.product = parse_product(child(root, "product"));
  result.rig = parse_rig(child(root, "rig"));
  result.capture = parse_capture(child(root, "capture"));
  result.rectification = parse_rectification(child(root, "rectification"));
  result.stereo = parse_stereo(child(root, "stereo"));
  result.outputs = parse_outputs(child(root, "outputs"));
  return result;
}

EffectiveConfig normalize(const LegacyProfileV1& legacy) {
  EffectiveConfig result;
  result.source_schema_version = legacy.schema_version;
  result.product = {legacy.product.platform, legacy.product.role,
                    legacy.product.profile_id};
  result.rig = {legacy.rig.sensor, legacy.rig.camera_count,
                legacy.rig.baseline_m, legacy.rig.calibration_status};
  const auto& capture = legacy.capture;
  const auto& trigger = capture.hardware_trigger;
  const auto& exposure = capture.auto_exposure;
  result.capture = {
      capture.backend,
      capture.left_camera_index,
      capture.right_camera_index,
      capture.width,
      capture.height,
      capture.stream_profile,
      capture.main_pixel_format,
      capture.depth_plane,
      capture.left_color_stream,
      capture.left_color_pixel_format,
      capture.left_color_memory_order,
      capture.right_color_stream_enabled,
      capture.colour_space,
      capture.orientation_degrees,
      capture.target_hz,
      capture.buffer_count,
      capture.handoff,
      capture.zero_copy_camera_input,
      {trigger.enabled,
       trigger.driver_mode,
       trigger.requires_always_on_overlay,
       trigger.gpiochip,
       trigger.line,
       trigger.idle_level,
       trigger.active_level,
       trigger.pulse_width_us,
       trigger.startup_priming_pulses,
       trigger.pair_tolerance_us},
      capture.exposure_time_us,
      capture.analogue_gain,
      capture.colour_gains,
      {exposure.enabled,
       exposure.metering_roi,
       exposure.target_median,
       exposure.median_deadband,
       exposure.dark_threshold,
       exposure.clipping_threshold,
       exposure.max_clipped_fraction,
       exposure.min_shutter_us,
       exposure.max_shutter_us,
       exposure.min_analogue_gain,
       exposure.max_analogue_gain,
       exposure.update_interval_frames,
       exposure.settle_frames_after_update}};
  const auto& rectification = legacy.rectification;
  result.rectification = {
      rectification.backend,
      rectification.fallback_backend,
      rectification.allow_fallback,
      rectification.left_calibration,
      rectification.right_calibration,
      rectification.map_format,
      rectification.depth_preprocessing,
      rectification.color_rectification};
  const auto& stereo = legacy.stereo;
  result.stereo = {
      stereo.backend,
      stereo.baseline_m,
      stereo.rectified_focal_length_px,
      stereo.output_width,
      stereo.output_height,
      stereo.min_disparity,
      stereo.max_disparity,
      stereo.working_width,
      stereo.working_height,
      stereo.working_disparities,
      stereo.block_size,
      stereo.mode,
      stereo.uniqueness_ratio,
      stereo.speckle_window_size,
      stereo.speckle_range,
      stereo.disparity_12_max_diff,
      stereo.pre_filter_cap,
      stereo.opencv_threads};
  result.outputs = {legacy.outputs.disparity_hz,
                    legacy.outputs.metric_depth_enabled,
                    legacy.outputs.compressed_image_hz,
                    legacy.outputs.jpeg_quality,
                    legacy.outputs.obstacle_cloud_enabled,
                    legacy.outputs.result_ring_path,
                    legacy.outputs.result_ring_slots,
                    legacy.outputs.result_ring_left_mono_enabled};
  result.legacy_ros = {legacy.outputs.disparity_topic,
                       legacy.outputs.compressed_image_topic,
                       legacy.outputs.left_camera_info_topic,
                       legacy.outputs.diagnostics_topic,
                       legacy.outputs.obstacle_cloud_topic};
  return result;
}

void validate(const EffectiveConfig& config) {
  if (config.schema_version != kCurrentEffectiveSchemaVersion) {
    validation_error("schema_version", "unsupported effective schema version");
  }
  if (config.source_schema_version != 1U) {
    validation_error("source_schema_version", "unsupported source schema version");
  }

  require_equal(config.product.platform, "raspberry-pi-5", "product.platform");
  require_equal(config.product.role, "standalone_depth_camera", "product.role");
  if (config.product.profile_id.empty()) {
    validation_error("product.profile_id", "must not be empty");
  }
  require_equal(config.rig.sensor, "imx296", "rig.sensor");
  if (config.rig.camera_count != 2U) {
    validation_error("rig.camera_count", "stereo schema v1 requires exactly 2");
  }
  require_positive(config.rig.baseline_m, "rig.baseline_m");

  const auto& capture = config.capture;
  require_equal(capture.backend, "libcamera", "capture.backend");
  if (capture.left_camera_index == capture.right_camera_index) {
    validation_error("capture.right_camera_index",
                     "must differ from capture.left_camera_index");
  }

  // Perform overflow checks before friendly size ceilings so hostile maximum
  // values exercise checked arithmetic instead of wrapping intermediate sizes.
  const auto capture_pixels = checked_multiply(
      capture.width, capture.height, "capture.width*capture.height");
  const auto disparity_bytes = checked_multiply(
      capture_pixels, sizeof(float), "capture disparity frame bytes");
  (void)checked_multiply(disparity_bytes, config.outputs.result_ring_slots,
                         "outputs.result_ring_slots frame storage");

  if (capture.width == 0U || capture.height == 0U ||
      capture.width > kMaxImageDimension ||
      capture.height > kMaxImageDimension) {
    validation_error("capture.width/capture.height",
                     "dimensions must be in the range 1..16384");
  }
  require_equal(capture.stream_profile, "isp_luma_rgb_left",
                "capture.stream_profile");
  require_equal(capture.main_pixel_format, "YUV420",
                "capture.main_pixel_format");
  require_equal(capture.depth_plane, "Y", "capture.depth_plane");
  require_equal(capture.left_color_stream, "lores",
                "capture.left_color_stream");
  require_equal(capture.left_color_pixel_format, "RGB888",
                "capture.left_color_pixel_format");
  require_equal(capture.left_color_memory_order, "BGR",
                "capture.left_color_memory_order");
  require_equal(capture.colour_space, "sYCC", "capture.colour_space");
  if (capture.orientation_degrees != 0U &&
      capture.orientation_degrees != 90U &&
      capture.orientation_degrees != 180U &&
      capture.orientation_degrees != 270U) {
    validation_error("capture.orientation_degrees",
                     "must be 0, 90, 180, or 270");
  }
  require_positive(capture.target_hz, "capture.target_hz");
  if (capture.target_hz > 1'000.0) {
    validation_error("capture.target_hz", "exceeds structural ceiling of 1000 Hz");
  }
  if (capture.buffer_count < 2U || capture.buffer_count > kMaxBufferCount) {
    validation_error("capture.buffer_count", "must be in the range 2..64");
  }
  require_equal(capture.handoff, "async_latest_pair", "capture.handoff");
  if (capture.colour_gains.size() != 2U ||
      std::ranges::any_of(capture.colour_gains,
                          [](const double gain) { return gain <= 0.0; })) {
    validation_error("capture.colour_gains",
                     "requires two finite values greater than zero");
  }
  require_positive(capture.analogue_gain, "capture.analogue_gain");

  const auto period_us = 1'000'000.0 / capture.target_hz;
  const auto& trigger = capture.hardware_trigger;
  if (trigger.enabled) {
    require_equal(trigger.driver_mode, "external_xtrig",
                  "capture.hardware_trigger.driver_mode");
    validate_path(trigger.gpiochip, "capture.hardware_trigger.gpiochip");
    require_one_of(trigger.idle_level, {"low", "high"},
                   "capture.hardware_trigger.idle_level");
    require_one_of(trigger.active_level, {"low", "high"},
                   "capture.hardware_trigger.active_level");
    if (trigger.idle_level == trigger.active_level) {
      validation_error("capture.hardware_trigger.active_level",
                       "must differ from idle_level");
    }
    if (trigger.pulse_width_us == 0U ||
        static_cast<double>(trigger.pulse_width_us) >= period_us) {
      validation_error("capture.hardware_trigger.pulse_width_us",
                       "must be positive and shorter than one capture period");
    }
    if (trigger.pair_tolerance_us == 0U ||
        static_cast<double>(trigger.pair_tolerance_us) >= period_us / 2.0) {
      validation_error("capture.hardware_trigger.pair_tolerance_us",
                       "must be positive and shorter than half a capture period");
    }
    if (capture.exposure_time_us != trigger.pulse_width_us) {
      validation_error(
          "capture.exposure_time_us",
          "schema v1 requires the configured exposure request to equal the "
          "initial trigger-low interval");
    }
  }
  if (capture.exposure_time_us == 0U ||
      static_cast<double>(capture.exposure_time_us) >= period_us) {
    validation_error("capture.exposure_time_us",
                     "must be positive and shorter than one capture period");
  }

  const auto& exposure = capture.auto_exposure;
  if (exposure.metering_roi.size() != 4U) {
    validation_error("capture.auto_exposure.metering_roi",
                     "must contain [x_min, y_min, x_max, y_max]");
  }
  const auto x_min = exposure.metering_roi[0];
  const auto y_min = exposure.metering_roi[1];
  const auto x_max = exposure.metering_roi[2];
  const auto y_max = exposure.metering_roi[3];
  if (x_min >= x_max || y_min >= y_max || x_max > capture.width ||
      y_max > capture.height) {
    validation_error("capture.auto_exposure.metering_roi",
                     "must define a nonempty rectangle within capture geometry");
  }
  if (exposure.median_deadband.size() != 2U ||
      exposure.median_deadband[0] > exposure.target_median ||
      exposure.target_median > exposure.median_deadband[1] ||
      exposure.median_deadband[1] > 255U) {
    validation_error("capture.auto_exposure.median_deadband",
                     "must contain target_median within an ordered 0..255 range");
  }
  if (exposure.dark_threshold > 255U ||
      exposure.clipping_threshold > 255U ||
      exposure.dark_threshold >= exposure.clipping_threshold) {
    validation_error("capture.auto_exposure.dark_threshold",
                     "dark threshold must be below clipping threshold in 0..255");
  }
  if (exposure.max_clipped_fraction < 0.0 ||
      exposure.max_clipped_fraction > 1.0) {
    validation_error("capture.auto_exposure.max_clipped_fraction",
                     "must be in the range 0..1");
  }
  if (exposure.min_shutter_us == 0U ||
      exposure.min_shutter_us > exposure.max_shutter_us ||
      static_cast<double>(exposure.max_shutter_us) >= period_us) {
    validation_error("capture.auto_exposure.min_shutter_us",
                     "shutter range must be positive, ordered, and below one period");
  }
  if (capture.exposure_time_us < exposure.min_shutter_us ||
      capture.exposure_time_us > exposure.max_shutter_us) {
    validation_error("capture.exposure_time_us",
                     "must be inside the auto-exposure shutter range");
  }
  if (exposure.min_analogue_gain <= 0.0 ||
      exposure.min_analogue_gain > exposure.max_analogue_gain) {
    validation_error("capture.auto_exposure.min_analogue_gain",
                     "gain range must be positive and ordered");
  }
  if (capture.analogue_gain < exposure.min_analogue_gain ||
      capture.analogue_gain > exposure.max_analogue_gain) {
    validation_error("capture.analogue_gain",
                     "must be inside the auto-exposure gain range");
  }
  if (exposure.enabled && exposure.update_interval_frames == 0U) {
    validation_error("capture.auto_exposure.update_interval_frames",
                     "must be positive while auto exposure is enabled");
  }

  const auto& rectification = config.rectification;
  require_one_of(rectification.backend, {"egl_gl_dmabuf", "cpu_opencv"},
                   "rectification.backend");
  require_one_of(rectification.fallback_backend,
                   {"egl_gl_dmabuf", "cpu_opencv"},
                   "rectification.fallback_backend");
  if (rectification.allow_fallback &&
      rectification.backend == rectification.fallback_backend) {
    validation_error("rectification.fallback_backend",
                     "must differ from backend when fallback is enabled");
  }
  validate_path(rectification.left_calibration,
                "rectification.left_calibration");
  validate_path(rectification.right_calibration,
                "rectification.right_calibration");
  if (rectification.left_calibration == rectification.right_calibration) {
    validation_error("rectification.right_calibration",
                     "must differ from left_calibration");
  }
  require_equal(rectification.map_format, "fixed_point",
                "rectification.map_format");
  require_equal(rectification.depth_preprocessing,
                "isp_luma_before_rectification",
                "rectification.depth_preprocessing");
  require_equal(rectification.color_rectification, "on_preview_schedule",
                "rectification.color_rectification");

  const auto& stereo = config.stereo;
  require_equal(stereo.backend, "cpu_sgbm", "stereo.backend");
  require_positive(stereo.baseline_m, "stereo.baseline_m");
  if (std::abs(stereo.baseline_m - config.rig.baseline_m) > 1e-12) {
    validation_error("stereo.baseline_m", "must equal rig.baseline_m");
  }
  require_positive(stereo.rectified_focal_length_px,
                   "stereo.rectified_focal_length_px");
  if (stereo.output_width == 0U || stereo.output_height == 0U ||
      stereo.working_width == 0U || stereo.working_height == 0U) {
    validation_error("stereo.output_width/stereo.output_height",
                     "output and working dimensions must be positive");
  }
  if (stereo.output_width != capture.width ||
      stereo.output_height != capture.height) {
    validation_error("stereo.output_width/stereo.output_height",
                     "schema v1 output geometry must equal capture geometry");
  }
  if (stereo.working_width > stereo.output_width ||
      stereo.working_height > stereo.output_height) {
    validation_error("stereo.working_width/stereo.working_height",
                     "working geometry must not exceed output geometry");
  }
  const auto horizontal_ratio = checked_multiply(
      stereo.output_width, stereo.working_height, "stereo geometry ratio");
  const auto vertical_ratio = checked_multiply(
      stereo.output_height, stereo.working_width, "stereo geometry ratio");
  if (horizontal_ratio != vertical_ratio) {
    validation_error("stereo.working_width/stereo.working_height",
                     "working and output geometries must have equal aspect ratio");
  }
  if (stereo.max_disparity <= stereo.min_disparity) {
    validation_error("stereo.max_disparity",
                     "must be greater than min_disparity");
  }
  if (stereo.working_disparities == 0U ||
      stereo.working_disparities % 16U != 0U) {
    validation_error("stereo.working_disparities",
                     "OpenCV SGBM requires a positive multiple of 16");
  }
  const auto output_scale = static_cast<double>(stereo.output_width) /
                            static_cast<double>(stereo.working_width);
  const auto configured_span = static_cast<double>(stereo.max_disparity) -
                               static_cast<double>(stereo.min_disparity);
  const auto searchable_span =
      static_cast<double>(stereo.working_disparities) * output_scale;
  if (configured_span > searchable_span + 1e-9) {
    validation_error("stereo.max_disparity",
                     "configured disparity span exceeds scaled working search");
  }
  if (stereo.block_size == 0U || stereo.block_size % 2U == 0U ||
      stereo.block_size > 255U) {
    validation_error("stereo.block_size", "must be an odd value in 1..255");
  }
  require_equal(stereo.mode, "sgbm_3way", "stereo.mode");
  if (stereo.uniqueness_ratio > 100U) {
    validation_error("stereo.uniqueness_ratio", "must be in the range 0..100");
  }
  if (stereo.disparity_12_max_diff < -1) {
    validation_error("stereo.disparity_12_max_diff", "must be -1 or greater");
  }
  if (stereo.pre_filter_cap == 0U || stereo.pre_filter_cap > 63U) {
    validation_error("stereo.pre_filter_cap", "must be in the range 1..63");
  }
  if (stereo.opencv_threads == 0U || stereo.opencv_threads > 64U) {
    validation_error("stereo.opencv_threads", "must be in the range 1..64");
  }

  const auto& outputs = config.outputs;
  require_positive(outputs.disparity_hz, "outputs.disparity_hz");
  require_positive(outputs.compressed_image_hz, "outputs.compressed_image_hz");
  if (outputs.disparity_hz > capture.target_hz + 1e-9) {
    validation_error("outputs.disparity_hz",
                     "must not exceed capture.target_hz");
  }
  if (outputs.compressed_image_hz > capture.target_hz + 1e-9) {
    validation_error("outputs.compressed_image_hz",
                     "must not exceed capture.target_hz");
  }
  if (outputs.jpeg_quality == 0U || outputs.jpeg_quality > 100U) {
    validation_error("outputs.jpeg_quality", "must be in the range 1..100");
  }
  validate_path(outputs.result_ring_path, "outputs.result_ring_path");
  if (outputs.result_ring_slots < 2U ||
      outputs.result_ring_slots > kMaxBufferCount) {
    validation_error("outputs.result_ring_slots", "must be in the range 2..64");
  }

  validate_topic(config.legacy_ros.disparity_topic,
                 "outputs.disparity_topic");
  validate_topic(config.legacy_ros.compressed_image_topic,
                 "outputs.compressed_image_topic");
  validate_topic(config.legacy_ros.left_camera_info_topic,
                 "outputs.left_camera_info_topic");
  validate_topic(config.legacy_ros.diagnostics_topic,
                 "outputs.diagnostics_topic");
  validate_topic(config.legacy_ros.obstacle_cloud_topic,
                 "outputs.obstacle_cloud_topic");
}

std::string canonical_json(const EffectiveConfig& config) {
  validate(config);
  return serialize_effective(config);
}

std::string sha256_hex(const std::string_view bytes) {
  Sha256 hash;
  hash.update(bytes);
  const auto digest = hash.finish();
  constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(digest.size() * 2U);
  for (const auto byte : digest) {
    output.push_back(hex[(byte >> 4U) & 0x0fU]);
    output.push_back(hex[byte & 0x0fU]);
  }
  return output;
}

LoadedConfig load(const std::filesystem::path& path) {
  auto effective = normalize(load_legacy_v1(path));
  validate(effective);
  const auto serialized = serialize_effective(effective);

  LoadedConfig result;
  result.effective = std::move(effective);
  result.configuration_sha256 = sha256_hex(serialized);
  if (result.effective.rig.calibration_status ==
      CalibrationStatus::kProvisionalRecalibrationRequired) {
    result.warnings.push_back(
        {"rig.calibration_status",
         "provisional calibration is structurally valid but does not authorize "
         "factory-calibrated metric depth"});
  }
  result.warnings.push_back(
      {"outputs.*_topic",
       "ROS topic fields are retained as legacy adapter configuration and do "
       "not define native daemon behavior"});
  if (result.effective.capture.hardware_trigger.enabled) {
    result.warnings.push_back(
        {"capture.hardware_trigger.pulse_width_us",
         "structural validation does not verify GPIO timing, sensor exposure, "
         "or the hardware-specific trigger/exposure offset"});
  }
  return result;
}

std::string dump_json(const LoadedConfig& loaded) {
  const auto configuration = canonical_json(loaded.effective);
  if (sha256_hex(configuration) != loaded.configuration_sha256) {
    throw std::runtime_error("effective configuration digest is inconsistent");
  }

  std::string output;
  output.reserve(configuration.size() + 1024U);
  output.push_back('{');
  bool first = true;
  append_integer_field(output, first, "effective_schema_version",
                       loaded.effective.schema_version);
  append_integer_field(output, first, "source_schema_version",
                       loaded.effective.source_schema_version);
  append_string_field(output, first, "configuration_sha256",
                      loaded.configuration_sha256);
  append_key(output, first, "warnings");
  output.push_back('[');
  for (std::size_t index = 0; index < loaded.warnings.size(); ++index) {
    if (index != 0U) {
      output.push_back(',');
    }
    output.push_back('{');
    bool warning_first = true;
    append_string_field(output, warning_first, "path", loaded.warnings[index].path);
    append_string_field(output, warning_first, "message",
                        loaded.warnings[index].message);
    output.push_back('}');
  }
  output.push_back(']');
  append_key(output, first, "configuration");
  output += configuration;
  output.push_back('}');
  return output;
}

}  // namespace depthd::config
