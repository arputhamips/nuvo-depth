#include "depthd/capture.hpp"
#include "depthd/config.hpp"

#if defined(DEPTHD_WITH_NATIVE_PIPELINE)
#include "depthd/calibration_store.hpp"
#include "depthd/exposure_control.hpp"
#include "depthd/frame_wire.hpp"
#include "depthd/ipc_v2.hpp"
#include "depthd/native_capture_runner.hpp"
#include "depthd/runtime_factory.hpp"
#include "depthd/opencv_rectification_backend.hpp"
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
#include "depthd/image_encoder.hpp"
#endif

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#endif

#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
#include "depthd/local_frame_server.hpp"
#endif

#if defined(DEPTHD_WITH_SYSTEMD)
#include <systemd/sd-daemon.h>
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#if defined(__unix__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr int kUsageOrValidationError = 2;
constexpr int kIoError = 3;
constexpr int kHardwareError = 4;
constexpr int kInternalError = 70;

struct CaptureCheckArguments {
  std::filesystem::path config_path;
  std::uint32_t pairs{1'000U};
  std::uint32_t cycles{2U};
  std::uint32_t pair_timeout_ms{3'000U};
  bool allow_userspace_trigger{};
};

#if defined(DEPTHD_WITH_NATIVE_PIPELINE)
struct PipelineArguments {
  std::filesystem::path config_path;
  std::filesystem::path calibration_root;
  std::filesystem::path calibration_state_root{"/var/lib/nuvo-depth"};
  std::filesystem::path left_image;
  std::filesystem::path right_image;
  std::filesystem::path output_directory;
  std::filesystem::path snapshot_directory{"/run/nuvo-depth/snapshots"};
  bool diagnostic_raw_snapshots{};
  std::filesystem::path encoding_config_path{
      "/etc/nuvo-depth/encoding.json"};
  std::filesystem::path stereo_control_path{
      "/etc/nuvo-depth/stereo.json"};
  std::filesystem::path exposure_control_path{
      "/etc/nuvo-depth/exposure.json"};
  std::filesystem::path status_path;
  std::filesystem::path trace_directory;
  std::filesystem::path calibration_request_directory{
      "/run/nuvo-depth/calibration/requests"};
  std::filesystem::path device_identity_path;
  std::uint32_t frames{};
  std::uint32_t pair_timeout_ms{3'000U};
  bool allow_provisional_depth{};
  bool live{};
};

std::atomic_bool stop_requested{};

extern "C" void request_stop(int) { stop_requested.store(true); }
#endif

void print_usage(std::ostream& stream) {
  stream << "Usage:\n"
         << "  depthd --check-config PATH\n"
         << "  depthd --dump-effective-config PATH\n"
         << "  depthd --capture-check PATH [--pairs N] [--cycles N] "
            "[--pair-timeout-ms N] [--allow-userspace-trigger]\n"
#if defined(DEPTHD_WITH_NATIVE_PIPELINE)
         << "  depthd --pipeline-check PATH --left IMAGE --right IMAGE "
            "--output-dir DIR [--calibration-root DIR] "
            "[--allow-provisional-depth]\n"
         << "  depthd --run PATH [--frames N] [--pair-timeout-ms N] "
            "[--status PATH] [--snapshot-dir DIR] [--calibration-root DIR] "
            "[--calibration-state-root DIR] [--encoding-config PATH] "
             "[--stereo-control PATH] [--exposure-control PATH] "
             "[--calibration-request-dir DIR] "
            "[--trace-dir DIR --frames N] "
            "--device-identity PATH [--allow-provisional-depth]\n"
#endif
      ;
}

std::uint32_t parse_positive_u32(const std::string_view value,
                                 const std::string_view option) {
  std::uint32_t parsed{};
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
      parsed == 0U) {
    throw std::invalid_argument(std::string(option) +
                                " requires a positive integer");
  }
  return parsed;
}

CaptureCheckArguments parse_capture_check(const int argc, char** argv) {
  if (argc < 3) {
    throw std::invalid_argument("--capture-check requires a configuration path");
  }
  CaptureCheckArguments result;
  result.config_path = argv[2];
  for (int index = 3; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--allow-userspace-trigger") {
      result.allow_userspace_trigger = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(argv[index]) +
                                  " requires a value");
    }
    const std::string_view value(argv[++index]);
    if (option == "--pairs") {
      result.pairs = parse_positive_u32(value, option);
    } else if (option == "--cycles") {
      result.cycles = parse_positive_u32(value, option);
    } else if (option == "--pair-timeout-ms") {
      result.pair_timeout_ms = parse_positive_u32(value, option);
    } else {
      throw std::invalid_argument("unknown capture-check option: " +
                                  std::string(option));
    }
  }
  if (result.pairs > 1'000'000U || result.cycles > 100U ||
      result.pair_timeout_ms > 60'000U) {
    throw std::invalid_argument("capture-check bound exceeds the safety limit");
  }
  return result;
}

#if defined(DEPTHD_WITH_NATIVE_PIPELINE)
PipelineArguments parse_pipeline_arguments(const int argc, char** argv,
                                           const bool live) {
  if (argc < 3) {
    throw std::invalid_argument(live ? "--run requires a configuration path"
                                     : "--pipeline-check requires a configuration path");
  }
  PipelineArguments result;
  result.config_path = argv[2];
  result.live = live;
  for (int index = 3; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--allow-provisional-depth") {
      result.allow_provisional_depth = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    const std::string_view value(argv[++index]);
    if (option == "--calibration-root") {
      result.calibration_root = value;
    } else if (option == "--calibration-state-root") {
      result.calibration_state_root = value;
    } else if (!live && option == "--left") {
      result.left_image = value;
    } else if (!live && option == "--right") {
      result.right_image = value;
    } else if (!live && option == "--output-dir") {
      result.output_directory = value;
    } else if (live && option == "--frames") {
      result.frames = parse_positive_u32(value, option);
    } else if (live && option == "--pair-timeout-ms") {
      result.pair_timeout_ms = parse_positive_u32(value, option);
    } else if (live && option == "--status") {
      result.status_path = value;
    } else if (live && option == "--device-identity") {
      result.device_identity_path = value;
    } else if (live && option == "--snapshot-dir") {
      result.snapshot_directory = value;
      result.diagnostic_raw_snapshots = true;
    } else if (live && option == "--encoding-config") {
      result.encoding_config_path = value;
    } else if (live && option == "--stereo-control") {
      result.stereo_control_path = value;
    } else if (live && option == "--exposure-control") {
      result.exposure_control_path = value;
    } else if (live && option == "--calibration-request-dir") {
      result.calibration_request_directory = value;
    } else if (live && option == "--trace-dir") {
      result.trace_directory = value;
    } else {
      throw std::invalid_argument("unknown pipeline option: " +
                                  std::string(option));
    }
  }
  if (!live && (result.left_image.empty() || result.right_image.empty() ||
                result.output_directory.empty())) {
    throw std::invalid_argument(
        "--pipeline-check requires --left, --right, and --output-dir");
  }
  if (result.frames > 1'000'000U || result.pair_timeout_ms > 60'000U) {
    throw std::invalid_argument("pipeline bound exceeds the safety limit");
  }
  if (!result.trace_directory.empty() &&
      (result.frames == 0U || result.frames > 120U)) {
    throw std::invalid_argument(
        "--trace-dir requires --frames in 1..120 to bound disk use");
  }
  if (live && result.device_identity_path.empty()) {
    throw std::invalid_argument("--run requires --device-identity");
  }
  return result;
}
#endif

std::string json_string(const std::string_view value) {
  std::ostringstream output;
  output << '"';
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20U) {
          constexpr char digits[] = "0123456789abcdef";
          output << "\\u00" << digits[character >> 4U]
                 << digits[character & 0x0fU];
        } else {
          output << static_cast<char>(character);
        }
    }
  }
  output << '"';
  return output.str();
}

#if defined(DEPTHD_WITH_NATIVE_PIPELINE)
depthd::CaptureOpenOptions capture_options(
    const depthd::config::EffectiveConfig& config);

struct CaptureTelemetry {
  std::int32_t left_exposure_us{};
  std::int32_t right_exposure_us{};
  float left_analogue_gain{};
  float right_analogue_gain{};
  float left_digital_gain{};
  float right_digital_gain{};
};

struct ExposureTelemetry {
  depthd::exposure::Configuration configuration;
  depthd::exposure::Command command;
  depthd::exposure::Stats stats;
};

struct NativeActivityTelemetry {
  depthd::ipc_v2::ProductMask raw_products{};
  double raw_cadence_hz{};
  double preview_cadence_hz{};
  std::uint64_t captured_pairs{};
  std::uint64_t raw_submissions{};
  std::uint64_t raw_cadence_skipped{};
  std::uint64_t idle_captures{};
  std::uint64_t statistics_capture_sequence{};
  std::uint64_t matched_frames{};
  std::uint64_t matcher_statistics_capture_sequence{};
  depthd::NativePipelineStats matcher_statistics{};
  std::size_t processed_payload_bytes{};
  bool capture_active{true};
  bool rectification_active{};
  bool matcher_active{};
  bool depth_conversion_active{};
  bool encoding_active{};
  depthd::CpuDmabufMappingStats mappings;
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
  depthd::encoding::Stats encoding;
#endif
};

struct NativeCaptureContext final : depthd::CapturePublicationContext {
  depthd::FrameSetIdentity identity;
  NativeActivityTelemetry activity;
  CaptureTelemetry capture;
  ExposureTelemetry exposure;
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
  depthd::encoding::CapturePlan encoding_plan;
#endif
};

depthd::exposure::Configuration exposure_fallback(
    const depthd::config::EffectiveConfig& config) {
  depthd::exposure::Configuration result;
  result.mode = config.capture.auto_exposure.enabled
                    ? depthd::exposure::Mode::kAuto
                    : depthd::exposure::Mode::kManual;
  result.manual_exposure_us = config.capture.exposure_time_us;
  result.manual_analogue_gain = config.capture.analogue_gain;
  result.red_gain = config.capture.colour_gains.at(0U);
  result.blue_gain = config.capture.colour_gains.at(1U);
  const auto& automatic = config.capture.auto_exposure;
  result.auto_target_median = automatic.target_median;
  result.auto_min_median = automatic.median_deadband.at(0U);
  result.auto_max_median = automatic.median_deadband.at(1U);
  result.auto_min_exposure_us = automatic.min_shutter_us;
  result.auto_max_exposure_us = automatic.max_shutter_us;
  result.auto_min_analogue_gain = automatic.min_analogue_gain;
  result.auto_max_analogue_gain = automatic.max_analogue_gain;
  result.auto_update_interval_frames = automatic.update_interval_frames;
  result.auto_settle_frames_after_update =
      automatic.settle_frames_after_update;
  depthd::exposure::validate(result);
  return result;
}

depthd::CaptureControls capture_controls(
    const depthd::exposure::Command& command) {
  depthd::CaptureControls result;
  result.exposure_time_us = command.exposure_us;
  result.analogue_gain = command.analogue_gain;
  result.colour_gains = {command.red_gain, command.blue_gain};
  depthd::validate(result);
  return result;
}

std::filesystem::path calibration_path(
    const PipelineArguments& arguments, const std::string& configured) {
  const auto configured_path = std::filesystem::path(configured);
  if (!arguments.calibration_root.empty()) {
    return arguments.calibration_root / configured_path.filename();
  }
  if (std::filesystem::exists(configured_path)) {
    return configured_path;
  }
  const auto development_path =
      arguments.config_path.parent_path() / "calibration" /
      "nuvo_depth_100mm" / configured_path.filename();
  return development_path;
}

depthd::PreparedStereoCalibration prepare_calibration(
    const PipelineArguments& arguments,
    const depthd::config::EffectiveConfig& configuration) {
  if (arguments.live) {
    const auto calibrations = arguments.calibration_state_root / "calibrations";
    const auto factory = calibrations / "factory" / "active";
    const auto user = calibrations / "user" / "active";
    if (std::filesystem::exists(factory) || std::filesystem::is_symlink(factory)) {
      return depthd::load_and_prepare_active_calibration(
          factory, depthd::CalibrationAuthority::kFactoryValidated,
          configuration.stereo.working_width,
          configuration.stereo.working_height);
    }
    if (std::filesystem::exists(user) || std::filesystem::is_symlink(user)) {
      return depthd::load_and_prepare_active_calibration(
          user, depthd::CalibrationAuthority::kUserValidated,
          configuration.stereo.working_width,
          configuration.stereo.working_height);
    }
  }
  return depthd::load_and_prepare_stereo_calibration(
      calibration_path(arguments, configuration.rectification.left_calibration),
      calibration_path(arguments, configuration.rectification.right_calibration),
      configuration.stereo.working_width,
      configuration.stereo.working_height);
}

depthd::RuntimeFactoryOptions runtime_options(
    const PipelineArguments& arguments) {
  depthd::RuntimeFactoryOptions result;
  result.allow_provisional_depth_for_engineering =
      arguments.allow_provisional_depth;
  if (arguments.live) {
    result.stereo_control_file = arguments.stereo_control_path.string();
  }
  return result;
}

std::string uuid_json(const std::array<std::uint8_t, 16>& value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(36U);
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U) {
      result.push_back('-');
    }
    result.push_back(digits[value[index] >> 4U]);
    result.push_back(digits[value[index] & 0x0fU]);
  }
  return result;
}

depthd::StereoProcessingRequest replay_request(
    const cv::Mat& left, const cv::Mat& right) {
  if (left.empty() || right.empty() || left.type() != CV_8UC1 ||
      right.type() != CV_8UC1 || left.size() != right.size()) {
    throw std::invalid_argument(
        "replay images must be equal-sized 8-bit grayscale images");
  }
  auto left_bytes = std::make_shared<std::vector<std::uint8_t>>(
      left.total());
  auto right_bytes = std::make_shared<std::vector<std::uint8_t>>(
      right.total());
  for (int row = 0; row < left.rows; ++row) {
    std::copy_n(left.ptr<std::uint8_t>(row), left.cols,
                left_bytes->data() + static_cast<std::size_t>(row) * left.cols);
    std::copy_n(right.ptr<std::uint8_t>(row), right.cols,
                right_bytes->data() + static_cast<std::size_t>(row) * right.cols);
  }
  const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  depthd::StereoPair pair;
  pair.capture_epoch = 1U;
  pair.pair_sequence = 1U;
  pair.capture_timestamp_ns = timestamp;
  pair.left.role = depthd::CameraRole::kLogicalLeft;
  pair.left.camera_stable_id = "replay-left";
  pair.left.capture_epoch = 1U;
  pair.left.stream_sequence = 1U;
  pair.left.sensor_timestamp_ns = timestamp;
  pair.left.sensor_clock = depthd::CaptureClock::kBoottime;
  pair.left.grid = depthd::make_reference_grid(
      static_cast<std::uint32_t>(left.cols),
      static_cast<std::uint32_t>(left.rows));
  pair.left.pixel_format = "GRAY8";
  pair.left.lease = depthd::CaptureBufferLease(1U, 1U, [] {});
  pair.right.role = depthd::CameraRole::kLogicalRight;
  pair.right.camera_stable_id = "replay-right";
  pair.right.capture_epoch = 1U;
  pair.right.stream_sequence = 1U;
  pair.right.sensor_timestamp_ns = timestamp;
  pair.right.sensor_clock = depthd::CaptureClock::kBoottime;
  pair.right.grid = pair.left.grid;
  pair.right.pixel_format = "GRAY8";
  pair.right.lease = depthd::CaptureBufferLease(1U, 2U, [] {});

  depthd::StereoProcessingRequest result;
  result.capture = std::move(pair);
  result.left_mapping_owner = left_bytes;
  result.right_mapping_owner = right_bytes;
  result.left = {left_bytes->data(), static_cast<std::uint32_t>(left.cols),
                 static_cast<std::uint32_t>(left.rows),
                 static_cast<std::size_t>(left.cols),
                 depthd::ImageEncoding::kGray8};
  result.right = {right_bytes->data(), static_cast<std::uint32_t>(right.cols),
                  static_cast<std::uint32_t>(right.rows),
                  static_cast<std::size_t>(right.cols),
                  depthd::ImageEncoding::kGray8};
  return result;
}

void write_bytes(const std::filesystem::path& path, const void* data,
                 const std::size_t bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create output: " + path.string());
  }
  output.write(static_cast<const char*>(data),
               static_cast<std::streamsize>(bytes));
  if (!output) {
    throw std::runtime_error("cannot write output: " + path.string());
  }
}

std::string frame_json(const depthd::FrameSetStorage& frame,
                       const std::string_view status,
                       const std::uint64_t published_frames = 0U,
                       const std::uint64_t publication_sequence_gaps = 0U,
                       const ExposureTelemetry* exposure = nullptr,
                       const CaptureTelemetry* capture = nullptr,
                       const depthd::CaptureRuntimeStats* capture_runtime =
                           nullptr,
                       const std::string_view left_camera_id = {},
                       const std::string_view right_camera_id = {},
                       const std::string_view profile_id = {},
                       const double capture_target_hz = 0.0,
                       const depthd::SchedulerStats* scheduler = nullptr,
                       const NativeActivityTelemetry* activity = nullptr) {
  const auto& metadata = frame.metadata;
  std::ostringstream output;
  output << "{\"schema_version\":1,\"status\":" << json_string(status)
         << ",\"device_uuid\":"
         << json_string(uuid_json(metadata.frame.device_uuid))
         << ",\"run_uuid\":"
         << json_string(uuid_json(metadata.frame.run_uuid))
         << ",\"capture_epoch\":" << metadata.frame.capture_epoch
         << ",\"capture_sequence\":" << metadata.frame.capture_sequence
         << ",\"published_frames\":" << published_frames
         << ",\"publication_sequence_gaps\":" << publication_sequence_gaps
         << ",\"capture_sequence_gaps\":" << (capture_runtime ?
              capture_runtime->pairer.left_sequence_gaps + capture_runtime->pairer.right_sequence_gaps : 0U)
         << ",\"configuration_epoch\":"
         << metadata.frame.configuration_epoch
         << ",\"calibration_sha256\":"
         << json_string(depthd::calibration_identity_to_sha256(
                metadata.calibration))
         << ",\"calibration_authority\":"
         << static_cast<unsigned>(metadata.calibration_authority)
         << ",\"image_width\":" << metadata.left_rectified.grid.width
         << ",\"image_height\":" << metadata.left_rectified.grid.height
         << ",\"depth_width\":" << metadata.depth.grid.width
         << ",\"depth_height\":" << metadata.depth.grid.height
         << ",\"rectification_backend\":"
         << json_string(frame.rectification_backend)
         << ",\"rectification_fallback_reason\":"
         << json_string(frame.rectification_fallback_reason)
         << ",\"stereo_backend\":" << json_string(frame.stereo_backend)
         << ",\"stereo_configuration_generation\":"
         << frame.stereo_configuration.generation
         << ",\"stereo_requested_revision\":"
         << json_string(frame.stereo_configuration.requested_revision)
         << ",\"stereo_applied_revision\":"
         << json_string(frame.stereo_configuration.applied_revision)
         << ",\"stereo_configuration_error\":"
         << json_string(frame.stereo_configuration.rejection_error)
         << ",\"rectification_ms\":" << frame.stats.rectification.total_ms
         << ",\"rectification_input_transfer_ms\":" << frame.stats.rectification.input_transfer_ms
         << ",\"rectification_compute_ms\":" << frame.stats.rectification.compute_ms
         << ",\"rectification_input_bytes_copied\":" << frame.stats.rectification.input_bytes_copied
         << ",\"stereo_preprocess_ms\":"
         << frame.stats.stereo.preprocess_ms
         << ",\"stereo_match_ms\":" << frame.stats.stereo.match_ms
         << ",\"stereo_postprocess_ms\":"
         << frame.stats.stereo.postprocess_ms
         << ",\"stereo_ms\":" << frame.stats.stereo.total_ms
         << ",\"valid_fraction\":" << frame.stats.stereo.valid_fraction
         << ",\"searchable_valid_fraction\":"
         << frame.stats.stereo.searchable_valid_fraction
         << ",\"valid_depth_pixels\":" << frame.stats.depth.valid_count
         << ",\"processing_ms\":" << frame.stats.total_ms
         << ",\"payload_bytes\":" << (activity != nullptr
             ? activity->processed_payload_bytes : frame.payload_bytes());
  if (!left_camera_id.empty() && !right_camera_id.empty()) {
    output << ",\"left_camera_id\":" << json_string(left_camera_id)
           << ",\"right_camera_id\":" << json_string(right_camera_id);
  }
  if (!profile_id.empty()) {
    output << ",\"profile_id\":" << json_string(profile_id);
  }
  if (capture_target_hz > 0.0) {
    output << ",\"capture_target_hz\":" << capture_target_hz;
  }
  if (exposure != nullptr) {
    const auto& configuration = exposure->configuration;
    const auto& command = exposure->command;
    const auto& stats = exposure->stats;
    output << ",\"exposure_mode\":"
           << json_string(depthd::exposure::name(configuration.mode))
           << ",\"requested_exposure_us\":" << command.exposure_us
           << ",\"requested_analogue_gain\":" << command.analogue_gain
           << ",\"requested_control_scope\":\"before_pair_observation\""
           << ",\"exposure_controller_state\":" << json_string(stats.state)
           << ",\"exposure_frame_period_us\":" << stats.frame_period_us
           << ",\"exposure_trigger_guard_us\":" << stats.trigger_guard_us
           << ",\"exposure_safe_max_us\":" << stats.safe_max_exposure_us
           << ",\"exposure_settle_frames_remaining\":"
           << stats.settle_frames_remaining
           << ",\"exposure_applied_controls_tracked\":" << (stats.applied_controls_tracked ? "true" : "false")
           << ",\"exposure_applied_controls_available\":" << (stats.applied_controls_available ? "true" : "false")
           << ",\"exposure_applied_controls_match\":" << (stats.applied_controls_match ? "true" : "false")
           << ",\"exposure_applied_controls_wait_frames\":" << stats.applied_controls_wait_frames
           << ",\"exposure_confirmation_observations\":"
           << stats.confirmation_observations
           << ",\"exposure_confirmation_direction\":"
           << json_string(stats.confirmation_direction)
           << ",\"exposure_at_lower_limit\":"
           << (stats.at_lower_limit ? "true" : "false")
           << ",\"exposure_at_upper_limit\":"
           << (stats.at_upper_limit ? "true" : "false")
           << ",\"exposure_last_adjustment_factor\":"
           << stats.last_adjustment_factor
           << ",\"exposure_updates\":" << stats.updates
           << ",\"exposure_frames_seen\":" << stats.frames_seen
           << ",\"exposure_observations\":" << stats.observations
           << ",\"exposure_configuration_reloads\":"
           << stats.configuration_reloads
           << ",\"exposure_configuration_errors\":"
           << stats.configuration_errors << ",\"exposure_last_reason\":"
           << json_string(stats.last_reason)
           << ",\"exposure_last_error\":" << json_string(stats.last_error)
           << ",\"metered_median\":";
    if (stats.median) output << *stats.median;
    else output << "null";
    output << ",\"metered_left_median\":";
    if (stats.left_median) output << *stats.left_median;
    else output << "null";
    output << ",\"metered_right_median\":";
    if (stats.right_median) output << *stats.right_median;
    else output << "null";
    output << ",\"metered_median_difference\":";
    if (stats.median_difference) output << *stats.median_difference;
    else output << "null";
    output << ",\"metered_dark_fraction\":";
    if (stats.dark_fraction) output << *stats.dark_fraction;
    else output << "null";
    output << ",\"metered_clipped_fraction\":";
    if (stats.clipped_fraction) output << *stats.clipped_fraction;
    else output << "null";
    output << ",\"metered_left_clipped_fraction\":";
    if (stats.left_clipped_fraction) output << *stats.left_clipped_fraction;
    else output << "null";
    output << ",\"metered_right_clipped_fraction\":";
    if (stats.right_clipped_fraction) output << *stats.right_clipped_fraction;
    else output << "null";
  }
  if (scheduler != nullptr) {
    output << ",\"scheduler_accepted\":" << scheduler->accepted
           << ",\"scheduler_pending_replaced\":"
           << scheduler->pending_replaced
           << ",\"scheduler_protected_pending_dropped\":" << scheduler->protected_pending_dropped
           << ",\"scheduler_processed\":" << scheduler->processed
           << ",\"scheduler_processing_failures\":"
           << scheduler->processing_failures
           << ",\"scheduler_storage_exhausted_dropped\":"
           << scheduler->storage_exhausted_dropped
           << ",\"scheduler_completed_dropped\":"
           << scheduler->completed_dropped
           << ",\"scheduler_pending\":" << scheduler->pending
           << ",\"scheduler_completed\":" << scheduler->completed;
  }
  if (capture != nullptr) {
    output << ",\"left_reported_exposure_us\":" << capture->left_exposure_us
           << ",\"right_reported_exposure_us\":" << capture->right_exposure_us
           << ",\"left_reported_analogue_gain\":"
           << capture->left_analogue_gain
           << ",\"right_reported_analogue_gain\":"
           << capture->right_analogue_gain
           << ",\"left_reported_digital_gain\":" << capture->left_digital_gain
           << ",\"right_reported_digital_gain\":"
           << capture->right_digital_gain;
  }
  if (capture_runtime != nullptr) {
    output << ",\"trigger_source\":" << json_string(capture_runtime->trigger_source)
           << ",\"trigger_hardware_timed\":" << (capture_runtime->trigger_hardware_timed ? "true" : "false")
           << ",\"trigger_pulse_timing_measured\":" << (capture_runtime->trigger_pulse_timing_measured ? "true" : "false")
           << ",\"trigger_commanded_pulse_us\":" << capture_runtime->trigger_commanded_pulse_us
           << ",\"trigger_period_ns\":" << capture_runtime->trigger_period_ns
           << ",\"left_submitted\":" << capture_runtime->pairer.left_submitted
           << ",\"right_submitted\":" << capture_runtime->pairer.right_submitted
           << ",\"stale_dropped\":" << capture_runtime->pairer.stale_dropped
           << ",\"capacity_dropped\":" << capture_runtime->pairer.capacity_dropped
           << ",\"duplicate_sequence_rejected\":" << capture_runtime->pairer.duplicate_sequence_rejected
           << ",\"trigger_attempts\":" << capture_runtime->trigger_attempts
           << ",\"trigger_deadline_overruns\":"
           << capture_runtime->trigger_deadline_overruns
           << ",\"trigger_last_pulse_us\":"
           << capture_runtime->trigger_last_pulse_us
           << ",\"trigger_min_pulse_us\":"
           << capture_runtime->trigger_min_pulse_us
           << ",\"trigger_max_pulse_us\":"
           << capture_runtime->trigger_max_pulse_us
           << ",\"trigger_pulse_guard_overruns\":"
           << capture_runtime->trigger_pulse_guard_overruns
           << ",\"trigger_max_pulse_overshoot_us\":"
           << capture_runtime->trigger_max_pulse_overshoot_us
           << ",\"trigger_errors\":" << capture_runtime->trigger_errors
           << ",\"frame_completion_errors\":"
           << capture_runtime->frame_completion_errors
           << ",\"ready_pair_replacements\":"
           << capture_runtime->ready_pair_replacements
           << ",\"request_requeue_failures\":"
           << capture_runtime->request_requeue_failures
           << ",\"paired_captures\":" << capture_runtime->pairer.paired
           << ",\"maximum_pair_delta_ns\":"
           << capture_runtime->pairer.maximum_sensor_delta_ns;
    output << ",\"left_sensor_sequence_gaps\":" << capture_runtime->pairer.left_sequence_gaps
           << ",\"right_sensor_sequence_gaps\":" << capture_runtime->pairer.right_sequence_gaps;
  }
  if (activity != nullptr) {
    output << ",\"raw_product_demand\":" << activity->raw_products
           << ",\"cpu_dmabuf_mappings_started\":" << activity->mappings.mappings_started
           << ",\"cpu_dmabuf_mapping_cache_hits\":" << activity->mappings.mapping_cache_hits
           << ",\"cpu_dmabuf_cached_mappings\":" << activity->mappings.cached_mappings
           << ",\"cpu_dmabuf_cached_bytes\":" << activity->mappings.cached_bytes
           << ",\"raw_demand_cadence_hz\":" << activity->raw_cadence_hz
           << ",\"preview_demand_cadence_hz\":" << activity->preview_cadence_hz
           << ",\"captured_pairs\":" << activity->captured_pairs
           << ",\"raw_processing_submissions\":" << activity->raw_submissions
           << ",\"raw_cadence_skipped\":" << activity->raw_cadence_skipped
           << ",\"idle_captures\":" << activity->idle_captures
           << ",\"processing_statistics_capture_sequence\":" << activity->statistics_capture_sequence
           << ",\"matched_frames\":" << activity->matched_frames
           << ",\"matcher_statistics\":{\"capture_sequence\":" << activity->matcher_statistics_capture_sequence
           << ",\"processing_ms\":" << activity->matcher_statistics.total_ms
           << ",\"rectification_ms\":" << activity->matcher_statistics.rectification.total_ms
           << ",\"rectification_input_transfer_ms\":" << activity->matcher_statistics.rectification.input_transfer_ms
           << ",\"rectification_compute_ms\":" << activity->matcher_statistics.rectification.compute_ms
           << ",\"rectification_input_bytes_copied\":" << activity->matcher_statistics.rectification.input_bytes_copied
           << ",\"stereo_preprocess_ms\":" << activity->matcher_statistics.stereo.preprocess_ms
           << ",\"stereo_match_ms\":" << activity->matcher_statistics.stereo.match_ms
           << ",\"stereo_postprocess_ms\":" << activity->matcher_statistics.stereo.postprocess_ms << '}'
           << ",\"capture_active\":" << (activity->capture_active ? "true" : "false")
           << ",\"rectification_active\":" << (activity->rectification_active ? "true" : "false")
           << ",\"matcher_active\":" << (activity->matcher_active ? "true" : "false")
           << ",\"depth_conversion_active\":" << (activity->depth_conversion_active ? "true" : "false")
           << ",\"encoding_active\":" << (activity->encoding_active ? "true" : "false");
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
    output << ",\"encoding_configuration_generation\":" << activity->encoding.configuration_generation
           << ",\"encoding_requested_revision\":" << json_string(activity->encoding.requested_revision)
           << ",\"encoding_applied_revision\":" << json_string(activity->encoding.applied_revision)
           << ",\"encoding_configuration_error\":" << json_string(activity->encoding.configuration_error)
           << ",\"encoding_last_error\":" << json_string(activity->encoding.last_error)
           << ",\"encoding_preview_unavailable\":" << activity->encoding.preview_unavailable
           << ",\"encoding_image_admitted\":" << activity->encoding.image_admitted
           << ",\"encoding_image_copies\":" << activity->encoding.image_copies
           << ",\"encoding_image_pool_dropped\":" << activity->encoding.image_pool_dropped
           << ",\"encoding_image_rectified\":" << activity->encoding.image_rectified
           << ",\"encoding_image_color_conversions\":" << activity->encoding.image_color_conversions
           << ",\"encoding_preparation_ms\":" << activity->encoding.last_preparation_ms
           << ",\"encoding_queue_ms\":" << activity->encoding.last_queue_ms
           << ",\"encoding_total_ms\":" << activity->encoding.last_total_ms
           << ",\"encoding_timing_scope\":\"preparation_queue_encode\""
           << ",\"encoding_numeric_admitted\":" << activity->encoding.numeric_admitted
           << ",\"encoded_frames\":" << activity->encoding.encoded
           << ",\"encoded_cadence_limited\":" << activity->encoding.cadence_limited
           << ",\"encode_failures\":" << activity->encoding.failures;
#endif
  }
  output << '}';
  return output.str();
}

void write_replay_products(const std::filesystem::path& directory,
                           const depthd::FrameSetStorage& frame) {
  std::filesystem::create_directories(directory);
  const auto image_width = static_cast<int>(frame.metadata.left_rectified.grid.width);
  const auto image_height = static_cast<int>(frame.metadata.left_rectified.grid.height);
  const auto depth_width = static_cast<int>(frame.metadata.depth.grid.width);
  const auto depth_height = static_cast<int>(frame.metadata.depth.grid.height);
  if (!cv::imwrite((directory / "left_rectified.png").string(),
                   cv::Mat(image_height, image_width, CV_8UC1,
                           const_cast<std::uint8_t*>(frame.left_rectified.data()))) ||
      !cv::imwrite((directory / "right_rectified.png").string(),
                   cv::Mat(image_height, image_width, CV_8UC1,
                           const_cast<std::uint8_t*>(frame.right_rectified.data()))) ||
      !cv::imwrite((directory / "confidence.png").string(),
                   cv::Mat(depth_height, depth_width, CV_8UC1,
                           const_cast<std::uint8_t*>(frame.confidence.data()))) ||
      !cv::imwrite((directory / "validity.png").string(),
                   cv::Mat(depth_height, depth_width, CV_8UC1,
                           const_cast<std::uint8_t*>(frame.validity.data())))) {
    throw std::runtime_error("OpenCV failed to write replay output images");
  }
  const cv::Mat validity(
      depth_height, depth_width, CV_8UC1,
      const_cast<std::uint8_t*>(frame.validity.data()));
  cv::Mat valid_mask;
  cv::compare(validity,
              static_cast<std::uint8_t>(depthd::ValidityCode::kValid),
              valid_mask, cv::CMP_EQ);
  const auto make_colormap = [&](const std::vector<float>& values,
                                 const float minimum, const float maximum,
                                 const std::filesystem::path& path) {
    cv::Mat normalized(depth_height, depth_width, CV_8UC1, cv::Scalar(0));
    const auto scale = 255.0F / (maximum - minimum);
    for (int y = 0; y < depth_height; ++y) {
      auto* output = normalized.ptr<std::uint8_t>(y);
      const auto offset = static_cast<std::size_t>(y) * depth_width;
      for (int x = 0; x < depth_width; ++x) {
        const auto value = values[offset + static_cast<std::size_t>(x)];
        if (std::isfinite(value)) {
          output[x] = static_cast<std::uint8_t>(std::clamp(
              (value - minimum) * scale, 0.0F, 255.0F));
        }
      }
    }
    cv::Mat colour;
    cv::applyColorMap(normalized, colour, cv::COLORMAP_TURBO);
    colour.setTo(cv::Scalar(0, 0, 0), valid_mask == 0);
    if (!cv::imwrite(path.string(), colour)) {
      throw std::runtime_error("OpenCV failed to write product visualization");
    }
  };
  make_colormap(frame.disparity,
                frame.metadata.disparity.accepted_min_disparity_px,
                frame.metadata.disparity.accepted_max_disparity_px,
                directory / "disparity_color.png");
  make_colormap(frame.depth_m, frame.metadata.depth.minimum_depth_m,
                frame.metadata.depth.maximum_depth_m,
                directory / "depth_color.png");
  write_bytes(directory / "disparity.f32", frame.disparity.data(),
              frame.disparity.size() * sizeof(float));
  write_bytes(directory / "depth_m.f32", frame.depth_m.data(),
              frame.depth_m.size() * sizeof(float));
  const auto wire = depthd::wire::serialize(
      depthd::wire::make_frame_set_message(frame));
  write_bytes(directory / "frame.nuv", wire.data(), wire.size());
  const auto metadata = frame_json(frame, "pass") + "\n";
  write_bytes(directory / "frame.json", metadata.data(), metadata.size());
}

int run_pipeline_check(const PipelineArguments& arguments) {
  const auto loaded = depthd::config::load(arguments.config_path);
  const auto calibration = prepare_calibration(arguments, loaded.effective);
  const auto identity = depthd::make_runtime_identity(loaded);
  auto pipeline = depthd::make_native_pipeline(
      loaded.effective, calibration, identity, runtime_options(arguments));
  const auto left = cv::imread(arguments.left_image.string(), cv::IMREAD_GRAYSCALE);
  const auto right =
      cv::imread(arguments.right_image.string(), cv::IMREAD_GRAYSCALE);
  const auto frame = pipeline->process(replay_request(left, right));
  write_replay_products(arguments.output_directory, *frame);
  std::cout << frame_json(*frame, "pass") << '\n';
  return 0;
}

void write_status(const std::filesystem::path& path,
                  const depthd::FrameSetStorage& frame,
                  const std::string_view state = "running",
                  const std::uint64_t published_frames = 0U,
                  const std::uint64_t publication_sequence_gaps = 0U,
                  const ExposureTelemetry* exposure = nullptr,
                  const CaptureTelemetry* capture = nullptr,
                  const depthd::CaptureRuntimeStats* capture_runtime = nullptr,
                  const std::string_view left_camera_id = {},
                  const std::string_view right_camera_id = {},
                  const std::string_view profile_id = {},
                  const double capture_target_hz = 0.0,
                  const depthd::SchedulerStats* scheduler = nullptr,
                  const NativeActivityTelemetry* activity = nullptr) {
  if (path.empty()) {
    return;
  }
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  const auto document =
      frame_json(frame, state, published_frames, publication_sequence_gaps,
                 exposure, capture, capture_runtime, left_camera_id,
                 right_camera_id, profile_id, capture_target_hz, scheduler, activity) +
      "\n";
  write_bytes(temporary, document.data(), document.size());
  std::filesystem::rename(temporary, path);
}

void publish_snapshot(const std::filesystem::path& directory,
                      const depthd::FrameSetStorage& frame,
                      const std::uint64_t stream_sequence) {
  if (directory.empty()) {
    return;
  }
  std::filesystem::create_directories(directory);
  const auto message = depthd::wire::make_frame_set_message(
      frame, depthd::ipc_v2::kAllProducts, stream_sequence);
  const auto bytes = depthd::wire::serialize(message);
  const auto destination = directory / "latest.nuv";
  const auto temporary = directory / "latest.nuv.tmp";
  write_bytes(temporary, bytes.data(), bytes.size());
  std::filesystem::rename(temporary, destination);
}

void trace_isp_pair(const std::filesystem::path& directory,
                    depthd::StereoProcessingRequest& request) {
  if (directory.empty()) return;
  if (!request.dmabuf || !request.dmabuf->acquire_cpu_fallback) {
    throw std::runtime_error("ISP trace requires mappable camera DMA-BUFs");
  }
  const auto mapped = request.dmabuf->acquire_cpu_fallback();
  const auto sequence = request.capture.pair_sequence;
  const auto write_luma = [&](const char* side, const depthd::ImageView& image) {
    const cv::Mat luma(static_cast<int>(image.height),
                       static_cast<int>(image.width), CV_8UC1,
                       const_cast<std::uint8_t*>(image.data),
                       image.stride_bytes);
    if (!cv::imwrite((directory / (std::string("isp_") + side + "_" +
                                 std::to_string(sequence) + ".png")).string(),
                     luma)) {
      throw std::runtime_error("could not write ISP trace image");
    }
  };
  write_luma("left", mapped.left);
  write_luma("right", mapped.right);
  const auto& left = request.capture.left;
  const auto& right = request.capture.right;
  std::ostringstream metadata;
  metadata << "{\"capture_sequence\":" << sequence
           << ",\"sensor_delta_ns\":" << request.capture.sensor_delta_ns
           << ",\"left\":{\"exposure_us\":" << left.isp_exposure_time_us
           << ",\"analogue_gain\":" << left.isp_analogue_gain
           << ",\"digital_gain\":" << left.isp_digital_gain
           << ",\"sensor_timestamp_ns\":" << left.sensor_timestamp_ns
           << "},\"right\":{\"exposure_us\":" << right.isp_exposure_time_us
           << ",\"analogue_gain\":" << right.isp_analogue_gain
           << ",\"digital_gain\":" << right.isp_digital_gain
           << ",\"sensor_timestamp_ns\":" << right.sensor_timestamp_ns
           << "}}\n";
  const auto document = metadata.str();
  write_bytes(directory / ("isp_metadata_" + std::to_string(sequence) +
                             ".json"), document.data(), document.size());
}

[[nodiscard]] bool calibration_identifier(const std::string_view value,
                                          const bool session) {
  if (value.empty() || value.size() > (session ? 32U : 64U) ||
      (session && value.size() != 32U)) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [session](const char byte) {
    const auto character = static_cast<unsigned char>(byte);
    return session ? (std::isdigit(character) != 0 ||
                      (byte >= 'a' && byte <= 'f'))
                   : (std::isalnum(character) != 0 || byte == '_' ||
                      byte == '-');
  });
}

void fulfill_calibration_capture(
    const std::filesystem::path& request_directory,
    const std::filesystem::path& state_root,
    depthd::StereoProcessingRequest& request,
    const std::array<std::uint8_t, 16>& device_uuid,
    const std::array<std::uint8_t, 16>& run_uuid) {
  if (request_directory.empty() ||
      !std::filesystem::is_directory(request_directory)) {
    return;
  }
  std::filesystem::path selected;
  for (const auto& entry :
       std::filesystem::directory_iterator(request_directory)) {
    if (entry.is_regular_file() && !entry.is_symlink() && entry.path().extension() == ".request" &&
        (selected.empty() || entry.path().filename() < selected.filename())) {
      selected = entry.path();
    }
  }
  if (selected.empty()) return;
  try {
    const auto stem = selected.stem().string();
    const auto separator = stem.find("--");
    if (separator == std::string::npos ||
        stem.find("--", separator + 2U) != std::string::npos) {
      throw std::invalid_argument("malformed calibration capture request");
    }
    const auto session_id = stem.substr(0U, separator);
    const auto capture_id = stem.substr(separator + 2U);
    if (!calibration_identifier(session_id, true) ||
        !calibration_identifier(capture_id, false)) {
      throw std::invalid_argument("unsafe calibration capture identifier");
    }
    if (!request.dmabuf || !request.dmabuf->acquire_cpu_fallback) {
      throw std::runtime_error(
          "calibration capture requires mappable ISP luma planes");
    }
    const auto session_directory =
        state_root / "calibration-sessions" / session_id;
    if (!std::filesystem::is_regular_file(session_directory /
                                          "configuration.json")) {
      throw std::runtime_error("calibration session does not exist");
    }
    const auto output = session_directory / "captures";
    if (std::filesystem::is_symlink(session_directory) ||
        !std::filesystem::is_directory(output) || std::filesystem::is_symlink(output)) {
      throw std::runtime_error("unsafe calibration capture directory");
    }
    const auto read_reservation = [](const std::filesystem::path& path) {
      if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
          std::filesystem::file_size(path) > 256U) {
        throw std::runtime_error("calibration capture reservation is unavailable");
      }
      std::ifstream input(path);
      return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto reservation_path = output / (capture_id + ".reservation");
    const auto reservation = read_reservation(selected);
    std::istringstream fields(reservation);
    std::string nonce, extra;
    std::uint32_t width{}, height{};
    std::size_t maximum_image_bytes{};
    if (!(fields >> nonce >> width >> height >> maximum_image_bytes) || (fields >> extra) ||
        !calibration_identifier(nonce, true) || width < 64U || height < 64U ||
        width > 8192U || height > 8192U ||
        maximum_image_bytes != static_cast<std::size_t>(width) * height * 101U / 100U + height + 65536U) {
      throw std::runtime_error("malformed calibration capture reservation");
    }
    const auto verify_reservation = [&] {
      if (read_reservation(reservation_path) != reservation) {
        throw std::runtime_error("calibration capture reservation was canceled");
      }
    };
    verify_reservation();
    const auto durable = [](const std::filesystem::path& path, const bool directory = false) {
#if defined(__unix__)
      const int descriptor = ::open(path.c_str(), O_RDONLY | (directory ? O_DIRECTORY : 0));
      if (descriptor < 0) throw std::runtime_error("cannot open calibration publication for fsync");
      const int result = ::fsync(descriptor);
      ::close(descriptor);
      if (result != 0) throw std::runtime_error("cannot fsync calibration publication");
#else
      static_cast<void>(path); static_cast<void>(directory);
#endif
    };
    struct TemporaryCleanup {
      std::vector<std::filesystem::path> paths;
      ~TemporaryCleanup() {
        for (const auto& path : paths) {
          std::error_code ignored; std::filesystem::remove(path, ignored);
        }
      }
    } cleanup;
    const auto publish = [&](const std::filesystem::path& destination, const void* bytes, const std::size_t size) {
      verify_reservation();
      if (std::filesystem::exists(destination)) {
        throw std::runtime_error("calibration capture already has published assets");
      }
      const auto temporary = output / ("." + destination.filename().string() + "." + nonce);
      cleanup.paths.push_back(temporary);
      write_bytes(temporary, bytes, size);
      durable(temporary);
      verify_reservation();
      std::filesystem::rename(temporary, destination);
      durable(output, true);
    };
    if (request.capture.capture_epoch == 0U || request.capture.pair_sequence == 0U ||
        request.capture.capture_timestamp_ns <= 0 ||
        request.capture.left.capture_epoch != request.capture.capture_epoch ||
        request.capture.right.capture_epoch != request.capture.capture_epoch) {
      throw std::runtime_error("calibration capture frame identity is inconsistent");
    }
    std::ostringstream frame;
    frame << "{\"device_uuid\":" << json_string(uuid_json(device_uuid))
          << ",\"run_uuid\":" << json_string(uuid_json(run_uuid))
          << ",\"capture_epoch\":" << request.capture.capture_epoch
          << ",\"capture_sequence\":" << request.capture.pair_sequence
          << ",\"capture_timestamp_ns\":" << request.capture.capture_timestamp_ns << '}';
    const auto frame_identity = frame.str();
    const auto mapped = request.dmabuf->acquire_cpu_fallback();
    std::ostringstream assets;
    const auto write_image = [&](const char* side,
                                 const depthd::ImageView& image) {
      const auto destination = output / (capture_id + "-" + side + ".png");
      if (image.width != width || image.height != height || image.encoding != depthd::ImageEncoding::kGray8) {
        throw std::runtime_error("calibration luma dimensions disagree with the reserved profile");
      }
      const cv::Mat luma(static_cast<int>(image.height),
                         static_cast<int>(image.width), CV_8UC1,
                         const_cast<std::uint8_t*>(image.data),
                         image.stride_bytes);
      std::vector<unsigned char> encoded;
      if (!cv::imencode(".png", luma, encoded) || encoded.size() > maximum_image_bytes) {
        throw std::runtime_error("could not write calibration ISP image");
      }
      publish(destination, encoded.data(), encoded.size());
      assets << json_string(side) << ":{\"name\":" << json_string(destination.filename().string())
             << ",\"bytes\":" << encoded.size() << ",\"sha256\":"
             << json_string(depthd::config::sha256_hex(std::string_view(
                    reinterpret_cast<const char*>(encoded.data()), encoded.size())))
             << ",\"frame\":" << frame_identity << ",\"width\":" << width
             << ",\"height\":" << height << "},";
    };
    write_image("left", mapped.left);
    write_image("right", mapped.right);
    const auto& left = request.capture.left;
    const auto& right = request.capture.right;
    std::ostringstream metadata;
    metadata << "{\"schema_version\":1,\"capture_sequence\":"
             << request.capture.pair_sequence
             << ",\"frame\":" << frame_identity
             << ",\"sensor_delta_ns\":" << request.capture.sensor_delta_ns
             << ",\"left_camera_id\":"
             << json_string(left.camera_stable_id)
             << ",\"right_camera_id\":"
             << json_string(right.camera_stable_id)
             << ",\"left\":{\"exposure_us\":"
             << left.isp_exposure_time_us << ",\"analogue_gain\":"
             << left.isp_analogue_gain << ",\"digital_gain\":"
             << left.isp_digital_gain << ",\"sensor_timestamp_ns\":"
             << left.sensor_timestamp_ns
             << "},\"right\":{\"exposure_us\":"
             << right.isp_exposure_time_us << ",\"analogue_gain\":"
             << right.isp_analogue_gain << ",\"digital_gain\":"
             << right.isp_digital_gain << ",\"sensor_timestamp_ns\":"
             << right.sensor_timestamp_ns << "}}\n";
    const auto document = metadata.str();
    const auto metadata_path = output / (capture_id + ".json");
    if (document.size() > 16384U) throw std::runtime_error("calibration metadata exceeds its bound");
    publish(metadata_path, document.data(), document.size());
    assets << "\"metadata\":{\"name\":" << json_string(metadata_path.filename().string())
           << ",\"bytes\":" << document.size() << ",\"sha256\":"
           << json_string(depthd::config::sha256_hex(document))
           << ",\"frame\":" << frame_identity << '}';
    std::ostringstream complete;
    complete << "{\"schema_version\":1,\"state\":\"complete\",\"session_id\":" << json_string(session_id)
             << ",\"capture_id\":" << json_string(capture_id) << ",\"frame\":" << frame_identity
             << ",\"assets\":{" << assets.str() << "}}\n";
    const auto committed = complete.str();
    // This final, durable manifest is the only completion event.
    publish(output / (capture_id + ".complete.json"), committed.data(), committed.size());
    std::filesystem::remove(selected);
  } catch (const std::exception& error) {
    const auto destination = selected.string() + ".error";
    const std::string message = std::string(error.what()) + "\n";
    try {
      const auto temporary = destination + ".tmp." + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
      write_bytes(temporary, message.data(), message.size());
      std::filesystem::rename(temporary, destination);
      std::filesystem::remove(selected);
    } catch (const std::exception&) {
    }
    std::cerr << "calibration capture request failed: " << error.what()
              << '\n';
  }
}

void trace_depth_frame(const std::filesystem::path& directory,
                       const depthd::FrameSetStorage& frame) {
  if (directory.empty()) return;
  const auto sequence = frame.metadata.frame.capture_sequence;
  const auto message = depthd::wire::make_frame_set_message(
      frame, depthd::ipc_v2::kAllProducts, sequence);
  const auto bytes = depthd::wire::serialize(message);
  write_bytes(directory / ("post_depth_" + std::to_string(sequence) +
                            ".nuv"), bytes.data(), bytes.size());
}

int run_native_daemon(const PipelineArguments& arguments) {
  if (!arguments.trace_directory.empty()) {
    if (std::filesystem::exists(arguments.trace_directory) &&
        !std::filesystem::is_empty(arguments.trace_directory)) {
      throw std::invalid_argument("trace directory must be empty");
    }
    std::filesystem::create_directories(arguments.trace_directory);
  }
  const auto loaded = depthd::config::load(arguments.config_path);
  const auto calibration = prepare_calibration(arguments, loaded.effective);
  std::ifstream identity_input(arguments.device_identity_path);
  std::string device_uuid;
  if (!identity_input || !std::getline(identity_input, device_uuid) ||
      device_uuid.empty()) {
    throw std::runtime_error("cannot read provisioned device identity");
  }
  if ((!calibration.unit_id.empty() && calibration.unit_id != device_uuid) ||
      (!calibration.preset_id.empty() &&
       calibration.preset_id != loaded.effective.product.profile_id)) {
    throw std::runtime_error(
        "active calibration does not belong to this device and preset");
  }
  const auto identity = depthd::make_runtime_identity(loaded, device_uuid);
  auto lane_options = runtime_options(arguments);
  // Split the original eight-slot / 256 MiB allowance between the lanes.
  lane_options.configuration_epochs =
      std::make_shared<depthd::NativeConfigurationEpochs>(identity.configuration_epoch);
  lane_options.frame_pool_capacity = 4U;
  lane_options.frame_pool_payload_budget_bytes = 128U * 1024U * 1024U;
  lane_options.cpu_rectification_maps = depthd::prepare_opencv_rectification_maps(calibration.maps);
  const auto pool_bytes = [&](const std::size_t slots, const bool images) {
    return depthd::required_frame_pool_payload_bytes(calibration.image_view.grid,
        calibration.matching_view.grid, slots, images);
  };
  constexpr std::size_t combined_pool_budget = 256U * 1024U * 1024U;
  const auto fits_combined_budget = [&](const std::size_t slots) {
    const auto matcher = pool_bytes(slots, false);
    return matcher <= combined_pool_budget &&
           pool_bytes(slots, true) <= combined_pool_budget - matcher;
  };
  while (lane_options.frame_pool_capacity > 2U &&
         !fits_combined_budget(lane_options.frame_pool_capacity)) {
    --lane_options.frame_pool_capacity;
  }
  const auto matcher_budget = pool_bytes(lane_options.frame_pool_capacity, false);
  const auto image_budget = pool_bytes(lane_options.frame_pool_capacity, true);
  if (matcher_budget > combined_pool_budget || image_budget > combined_pool_budget - matcher_budget) {
    throw std::invalid_argument("capture geometry cannot fit two retained frames per lane within 256 MiB");
  }
  lane_options.frame_pool_payload_budget_bytes = matcher_budget;
  auto pipeline = depthd::make_native_pipeline(
      loaded.effective, calibration, identity, lane_options);
  lane_options.image_only = true;
  lane_options.frame_pool_payload_budget_bytes = image_budget;
  auto image_pipeline = depthd::make_native_pipeline(
      loaded.effective, calibration, identity, lane_options);
  depthd::BoundedNativeScheduler scheduler(
      {1U, 1U}, std::move(pipeline), std::move(image_pipeline));
#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
  depthd::ipc_v2::LocalFrameServer local_frames;
#endif
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
  depthd::encoding::Configuration encoder_fallback;
  // Missing or invalid runtime controls fail safe to the original raw stream.
  // The packaged configuration keeps encoding off until monitoring is needed.
  encoder_fallback.mode = depthd::encoding::StreamMode::kRawOnly;
  encoder_fallback.cadence_hz = std::max(
      1U, static_cast<std::uint32_t>(
              std::lround(loaded.effective.outputs.compressed_image_hz)));
  encoder_fallback.jpeg_quality = loaded.effective.outputs.jpeg_quality;
  depthd::encoding::AsyncFrameEncoder image_encoder(
      arguments.encoding_config_path,
      arguments.diagnostic_raw_snapshots ? arguments.snapshot_directory / "latest.nve" : std::filesystem::path{}, encoder_fallback,
      arguments.trace_directory, {}, [&] {
        depthd::RectificationBackendConfig preview_configuration;
        preview_configuration.input_width = loaded.effective.capture.width;
        preview_configuration.input_height = loaded.effective.capture.height;
        preview_configuration.output_width = calibration.image_view.grid.width;
        preview_configuration.output_height = calibration.image_view.grid.height;
        return depthd::make_opencv_rectification_backend(
            preview_configuration, lane_options.cpu_rectification_maps);
      }
#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
      , [&](const depthd::FrameSetIdentity& frame, std::uint64_t sequence,
            std::span<const std::byte> bytes) {
        local_frames.publish_encoded({frame.run_uuid, frame.capture_epoch, sequence}, bytes);
      }
#endif
      );
  image_encoder.set_demand(0.0);
#endif
  auto capture = depthd::make_pi5_libcamera_capture_source();
  if (!capture) {
    throw std::runtime_error("Pi capture backend " +
                             std::string(depthd::pi5_capture_backend_status()));
  }
  depthd::exposure::Controller exposure_controller(
      arguments.exposure_control_path, exposure_fallback(loaded.effective),
      {static_cast<std::uint32_t>(std::floor(
           1'000'000.0 / loaded.effective.capture.target_hz)),
       depthd::kExternalTriggerGuardUs},
      {std::array<std::uint32_t, 4>{loaded.effective.capture.auto_exposure.metering_roi.at(0),
          loaded.effective.capture.auto_exposure.metering_roi.at(1),
          loaded.effective.capture.auto_exposure.metering_roi.at(2),
          loaded.effective.capture.auto_exposure.metering_roi.at(3)},
       loaded.effective.capture.auto_exposure.dark_threshold,
       loaded.effective.capture.auto_exposure.clipping_threshold,
       loaded.effective.capture.auto_exposure.max_clipped_fraction});
  auto open_options = capture_options(loaded.effective);
  const auto initial_controls = capture_controls(exposure_controller.command());
  open_options.exposure_time_us = initial_controls.exposure_time_us;
  open_options.trigger_pulse_width_us = initial_controls.exposure_time_us;
  open_options.analogue_gain = initial_controls.analogue_gain;
  open_options.colour_gains = initial_controls.colour_gains;
  const auto session = capture->start(open_options);
  if ((!calibration.left_camera_id.empty() &&
       calibration.left_camera_id != session.left.binding.camera.stable_id) ||
      (!calibration.right_camera_id.empty() &&
       calibration.right_camera_id != session.right.binding.camera.stable_id)) {
    capture->stop();
    throw std::runtime_error(
        "active calibration does not match the bound logical cameras");
  }
  std::shared_ptr<const depthd::FrameSetStorage> last_frame;
  std::shared_ptr<const NativeCaptureContext> last_context;
  std::uint64_t published{};
  std::uint64_t matched_published{}, matched_statistics_sequence{};
  depthd::NativePipelineStats matched_statistics{};
  std::uint64_t publication_sequence_gaps{};
  std::uint64_t previous_publication_sequence{};
  std::mutex capture_status_mutex;
  std::shared_ptr<const NativeCaptureContext> newest_capture_context;
  NativeActivityTelemetry capture_activity;
  depthd::NativeCaptureCadence raw_capture_cadence;
  depthd::NativeCaptureCadence matcher_capture_cadence;
  auto next_calibration_scan = std::chrono::steady_clock::time_point{};
  auto next_status_write = std::chrono::steady_clock::time_point{};
#if defined(DEPTHD_WITH_SYSTEMD)
  bool ready_notified{};
#endif
  stop_requested.store(false);
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
  depthd::NativeCaptureRunner runner(
      *capture, scheduler,
      std::chrono::milliseconds(arguments.pair_timeout_ms),
      [&] {
        // The capture producer is the sole owner of control reloads and auto
        // exposure metering; publication uses the immutable per-pair snapshot.
        if (const auto updated = exposure_controller.refresh()) {
          capture->set_controls(capture_controls(*updated));
        }
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
        image_encoder.refresh_configuration();
#endif
      },
      [&](depthd::StereoPair pair) -> std::optional<depthd::StereoProcessingRequest> {
        auto context = std::make_shared<NativeCaptureContext>();
        context->identity = depthd::make_frame_set_identity(
            pair, identity.device_uuid, identity.run_uuid, identity.configuration_epoch);
        context->capture = {pair.left.isp_exposure_time_us,
                            pair.right.isp_exposure_time_us,
                            pair.left.isp_analogue_gain,
                            pair.right.isp_analogue_gain,
                            pair.left.isp_digital_gain,
                            pair.right.isp_digital_gain};
        depthd::ipc_v2::ProductMask products{};
        double raw_cadence{};
        double preview_cadence{};
#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
        const auto demand = local_frames.demand();
        products = demand.products;
        raw_cadence = demand.cadence_hz;
        preview_cadence = demand.preview_cadence_hz;
#endif
        // Explicit diagnostic outputs count as consumers. Normal production
        // has no such demand and performs no product work while unsubscribed.
        if (arguments.diagnostic_raw_snapshots || !arguments.trace_directory.empty()) {
          products |= depthd::ipc_v2::kAllProducts;
          raw_cadence = std::max(raw_cadence, loaded.effective.capture.target_hz);
        }
        const auto raw_product_demand = products;
        bool raw_due{};
        depthd::ipc_v2::ProductMask independent_images{};
        depthd::ipc_v2::ProductMask associated_images{};
#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
        bool image_due{}, matcher_due{};
        if (!arguments.diagnostic_raw_snapshots && arguments.trace_directory.empty()) {
          products = 0U;
          image_due = raw_capture_cadence.admit(context->identity, demand.image_products, demand.image_cadence_hz);
          matcher_due = matcher_capture_cadence.admit(context->identity, demand.matcher_products, demand.matcher_cadence_hz);
          if (image_due) {
            independent_images = demand.image_products;
            products |= independent_images;
          }
          if (matcher_due) {
            products |= demand.matcher_products;
            associated_images = demand.matcher_products & depthd::ipc_v2::kImageProducts;
          }
          raw_due = products != 0U;
        } else
#endif
        raw_due = raw_capture_cadence.admit(context->identity, products, raw_cadence);
        if (!raw_due) {
          if (raw_product_demand != 0U && raw_cadence > 0.0) ++capture_activity.raw_cadence_skipped;
          products = 0U;
        }
        auto processing = depthd::make_cpu_dmabuf_processing_request(std::move(pair));
        processing.products = products;
        const auto requested_at_capture = exposure_controller.command();
        exposure_controller.update_applied_controls({context->capture.left_exposure_us,
            context->capture.right_exposure_us, context->capture.left_analogue_gain,
            context->capture.right_analogue_gain});
        if (exposure_controller.should_meter_next_frame()) {
          if (!processing.dmabuf || !processing.dmabuf->acquire_cpu_fallback) {
            throw std::runtime_error("auto exposure requires mappable ISP luma planes");
          }
          const auto mapped = processing.dmabuf->acquire_cpu_fallback();
          // Reuse this interval's metering maps for an admitted mono preview
          // or CPU native product instead of mapping the same leased luma again.
          processing.left_mapping_owner = mapped.left_owner;
          processing.right_mapping_owner = mapped.right_owner;
          processing.left = mapped.left;
          processing.right = mapped.right;
          processing.dmabuf->left_cpu_fallback = mapped.left;
          processing.dmabuf->right_cpu_fallback = mapped.right;
          if (const auto command = exposure_controller.observe(mapped.left, mapped.right)) {
            capture->set_controls(capture_controls(*command));
          }
        } else {
          exposure_controller.skip_observation_frame();
        }
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
        const auto encoding = image_encoder.configuration();
        if (!depthd::encoding::encoded_enabled(encoding)) preview_cadence = 0.0;
        image_encoder.set_demand(preview_cadence);
        bool preview_due = preview_cadence > 0.0;
#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
        if (!arguments.diagnostic_raw_snapshots && arguments.trace_directory.empty()) {
          preview_due = depthd::preview_capture_aligned(
              std::min(preview_cadence, static_cast<double>(encoding.cadence_hz)),
              demand.matcher_cadence_hz, matcher_due, demand.image_cadence_hz, image_due,
              demand.paired_products, context->identity.capture_time_ns,
              matcher_capture_cadence.next_due_time_ns(), raw_capture_cadence.next_due_time_ns());
        }
#endif
        if (preview_due) {
          if (depthd::encoding::requires_depth(encoding.source)) {
            context->encoding_plan = image_encoder.prepare_capture(context->identity);
            if (context->encoding_plan.result == depthd::encoding::SubmitResult::kAccepted) {
              const auto kind = encoding.source == depthd::encoding::Source::kDepth
                  ? depthd::ipc_v2::ProductKind::kDepth : depthd::ipc_v2::ProductKind::kDisparity;
              products |= depthd::ipc_v2::product_bit(kind);
              processing.preserve_for_preview = true;
            }
          } else {
            depthd::ProductSetMetadata preview_metadata;
            preview_metadata.frame = context->identity;
            preview_metadata.calibration = calibration.identity;
            preview_metadata.calibration_authority = calibration.authority;
            preview_metadata.left_rectified.grid = calibration.image_view.grid;
            preview_metadata.right_rectified.grid = calibration.image_view.grid;
            (void)image_encoder.submit_capture(preview_metadata,
                [&](const depthd::CameraRole role, const bool color,
                    const std::span<std::uint8_t> destination) {
                  depthd::copy_capture_image(processing, role, color, destination);
                });
          }
        }
#endif
        processing.products = products;
#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
        processing.independent_image_products = independent_images;
        processing.associated_image_products = associated_images;
#endif
        trace_isp_pair(arguments.trace_directory, processing);
        const auto housekeeping_now = std::chrono::steady_clock::now();
        if (housekeeping_now >= next_calibration_scan) {
          fulfill_calibration_capture(arguments.calibration_request_directory,
                                      arguments.calibration_state_root, processing,
                                      identity.device_uuid, identity.run_uuid);
          next_calibration_scan = housekeeping_now + std::chrono::milliseconds(250);
        }
        context->exposure = {exposure_controller.configuration(),
                             requested_at_capture, exposure_controller.stats()};
        ++capture_activity.captured_pairs;
        capture_activity.raw_products = raw_product_demand;
        capture_activity.raw_cadence_hz = raw_cadence;
        capture_activity.preview_cadence_hz = preview_cadence;
        capture_activity.matcher_active = (products & depthd::ipc_v2::kMatcherProducts) != 0U;
        capture_activity.depth_conversion_active =
            (products & (depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kDepth) |
                         depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kValidity))) != 0U;
        constexpr auto rectified_products =
            depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kLeftRectified) |
            depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kRightRectified) |
            depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kLeftRectifiedRgb) |
            depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kRightRectifiedRgb) |
            depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kRectifiedStereo) |
            depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kRectifiedStereoRgb);
        capture_activity.rectification_active = capture_activity.matcher_active ||
            (products & rectified_products) != 0U;
        capture_activity.encoding_active = preview_cadence > 0.0;
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
        if (preview_cadence > 0.0) {
          const auto source = image_encoder.configuration().source;
          capture_activity.rectification_active |=
              source == depthd::encoding::Source::kLeftRectified ||
              source == depthd::encoding::Source::kRightRectified ||
              source == depthd::encoding::Source::kLeftRectifiedRgb ||
              source == depthd::encoding::Source::kRightRectifiedRgb ||
              source == depthd::encoding::Source::kRectifiedStereo ||
              source == depthd::encoding::Source::kRectifiedStereoRgb;
        }
#endif
        const bool submit_raw = products != 0U;
        if (submit_raw) {
          ++capture_activity.raw_submissions;
        } else if (raw_product_demand == 0U && preview_cadence == 0.0) {
          ++capture_activity.idle_captures;
        }
        context->activity = capture_activity;
        {
          std::lock_guard lock(capture_status_mutex);
          newest_capture_context = context;
        }
        if (!submit_raw) return std::nullopt;
        processing.publication_context = context;
        return processing;
      }, [&](const std::shared_ptr<const depthd::CapturePublicationContext> publication,
              const depthd::SubmitResult result) {
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
        if (result == depthd::SubmitResult::kAccepted) {
          const auto context = std::dynamic_pointer_cast<const NativeCaptureContext>(publication);
          if (context && context->encoding_plan.result == depthd::encoding::SubmitResult::kAccepted)
            (void)image_encoder.commit_native_capture(context->encoding_plan);
        }
#else
        (void)publication;
        (void)result;
#endif
      });
  try {
    while (!stop_requested.load() &&
           (arguments.frames == 0U || published < arguments.frames)) {
      auto frame = runner.next(std::chrono::milliseconds(100));
#if defined(DEPTHD_WITH_SYSTEMD)
      // Readiness still requires a coherent processed frame. Once ready,
      // bounded storage drops/source gaps must not starve the watchdog.
      if (ready_notified) {
        (void)sd_notify(0, "WATCHDOG=1");
      }
#endif
      if (frame) {
        const auto context = std::dynamic_pointer_cast<const NativeCaptureContext>(
            frame->publication_context);
        if (!context) throw std::runtime_error("processed frame lost its capture context");
        const auto sequence = frame->metadata.frame.capture_sequence;
        if (previous_publication_sequence != 0U && sequence > previous_publication_sequence + 1U) {
          publication_sequence_gaps += sequence - previous_publication_sequence - 1U;
        }
        previous_publication_sequence = std::max(previous_publication_sequence, sequence);
        ++published;
        if ((frame->available_products & depthd::ipc_v2::kMatcherProducts) != 0U) {
          ++matched_published; matched_statistics = frame->stats; matched_statistics_sequence = sequence;
        }
        trace_depth_frame(arguments.trace_directory, *frame);
#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
        local_frames.publish(frame);
#endif
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
        (void)image_encoder.submit_native(frame, context->encoding_plan);
        if (arguments.diagnostic_raw_snapshots &&
            depthd::encoding::raw_enabled(image_encoder.configuration())) {
          publish_snapshot(arguments.snapshot_directory, *frame, published);
        }
#else
        if (arguments.diagnostic_raw_snapshots) publish_snapshot(arguments.snapshot_directory, *frame, published);
#endif
        if (!last_frame || frame->metadata.frame.capture_time_ns >=
                               last_frame->metadata.frame.capture_time_ns) {
          last_frame = frame;
          last_context = context;
        }
      }
      std::shared_ptr<const NativeCaptureContext> latest_capture;
      {
        std::lock_guard lock(capture_status_mutex);
        latest_capture = newest_capture_context;
      }
      if (!latest_capture) continue;
      depthd::FrameSetStorage status_frame;
      if (last_frame) {
        status_frame.metadata = last_frame->metadata;
        status_frame.stereo_configuration = last_frame->stereo_configuration;
        status_frame.rectification_backend = last_frame->rectification_backend;
        status_frame.stereo_backend = last_frame->stereo_backend;
        status_frame.stats = last_frame->stats;
      }
      status_frame.metadata.frame = latest_capture->identity;
      status_frame.metadata.calibration = calibration.identity;
      status_frame.metadata.calibration_authority = calibration.authority;
      status_frame.metadata.left_rectified.grid = calibration.image_view.grid;
      status_frame.metadata.right_rectified.grid = calibration.image_view.grid;
      status_frame.metadata.depth.grid = calibration.matching_view.grid;
      auto activity = latest_capture->activity;
      activity.matched_frames = matched_published;
      activity.matcher_statistics = matched_statistics;
      activity.matcher_statistics_capture_sequence = matched_statistics_sequence;
      activity.mappings = depthd::cpu_dmabuf_mapping_stats();
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
      activity.encoding = image_encoder.stats();
#endif
      activity.statistics_capture_sequence = last_frame
          ? last_frame->metadata.frame.capture_sequence : 0U;
      activity.processed_payload_bytes = last_frame && activity.raw_products != 0U
          ? last_frame->payload_bytes() : 0U;
      if (!activity.matcher_active) {
        status_frame.stats.stereo = {};
        status_frame.stereo_backend = "not_requested";
      }
      if (!activity.depth_conversion_active) status_frame.stats.depth = {};
      if (!activity.rectification_active) {
        status_frame.stats.rectification = {};
        status_frame.rectification_backend = "not_requested";
      }
      const auto live_capture_stats = capture->runtime_stats();
      const auto live_scheduler_stats = scheduler.stats();
      const auto status_now = std::chrono::steady_clock::now();
      if (status_now >= next_status_write) {
        write_status(arguments.status_path, status_frame, "running", published,
                   publication_sequence_gaps, &latest_capture->exposure, &latest_capture->capture,
                   &live_capture_stats, session.left.binding.camera.stable_id,
                   session.right.binding.camera.stable_id, loaded.effective.product.profile_id,
                   loaded.effective.capture.target_hz, &live_scheduler_stats, &activity);
        next_status_write = status_now + std::chrono::milliseconds(250);
      }
#if defined(DEPTHD_WITH_SYSTEMD)
      if (!ready_notified) {
        (void)sd_notify(0, "READY=1\nSTATUS=Associated capture ready; outputs follow subscriber demand");
        ready_notified = true;
      }
      (void)sd_notify(0, "WATCHDOG=1");
#endif
      // A finite engineering image/idle run must also terminate without a raw
      // product subscriber. Diagnostic raw runs retain their published limit.
      if (arguments.frames != 0U && activity.raw_products == 0U &&
          activity.captured_pairs >= arguments.frames) break;
    }
  } catch (...) {
    // Processing requests retain camera mappings and request leases. Drain the
    // sole processing owner before capture tears down libcamera allocations.
    runner.stop();
    throw;
  }
  runner.stop();
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
  image_encoder.stop();
#endif
  const auto scheduler_stats = scheduler.stats();
  const auto capture_stats = capture->runtime_stats();
  const auto mapping_stats = depthd::cpu_dmabuf_mapping_stats();
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
  const auto encoder_config = image_encoder.configuration();
  const auto encoder_stats = image_encoder.stats();
#endif
  std::shared_ptr<const NativeCaptureContext> final_capture;
  {
    std::lock_guard lock(capture_status_mutex);
    final_capture = newest_capture_context;
  }
  if (final_capture) {
    depthd::FrameSetStorage final_status;
    if (last_frame) {
      final_status.metadata = last_frame->metadata;
      final_status.stereo_configuration = last_frame->stereo_configuration;
    }
    final_status.metadata.frame = final_capture->identity;
    final_status.metadata.calibration = calibration.identity;
    final_status.metadata.calibration_authority = calibration.authority;
    final_status.metadata.left_rectified.grid = calibration.image_view.grid;
    final_status.metadata.right_rectified.grid = calibration.image_view.grid;
    final_status.metadata.depth.grid = calibration.matching_view.grid;
    auto activity = final_capture->activity;
    activity.matched_frames = matched_published;
    activity.matcher_statistics = matched_statistics;
    activity.matcher_statistics_capture_sequence = matched_statistics_sequence;
    activity.mappings = mapping_stats;
    activity.capture_active = false;
    activity.rectification_active = false;
    activity.matcher_active = false;
    activity.depth_conversion_active = false;
    activity.encoding_active = false;
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
    activity.encoding = encoder_stats;
#endif
    write_status(arguments.status_path, final_status, "stopped", published,
                 publication_sequence_gaps, &final_capture->exposure, &final_capture->capture,
                 &capture_stats, session.left.binding.camera.stable_id,
                 session.right.binding.camera.stable_id, loaded.effective.product.profile_id,
                 loaded.effective.capture.target_hz, &scheduler_stats, &activity);
  }
  if (last_frame && arguments.diagnostic_raw_snapshots) {
    write_replay_products(arguments.snapshot_directory, *last_frame);
  }
#if defined(DEPTHD_WITH_SYSTEMD)
  (void)sd_notify(0, "STOPPING=1\nSTATUS=Stopped cleanly");
#endif
  std::cout << "{\"schema_version\":1,\"status\":\"stopped\""
            << ",\"capture_epoch\":" << session.capture_epoch
            << ",\"published_frames\":" << published
            << ",\"matched_frames\":" << matched_published
            << ",\"captured_pairs\":" << capture_activity.captured_pairs
            << ",\"raw_processing_submissions\":" << capture_activity.raw_submissions
            << ",\"raw_cadence_skipped\":" << capture_activity.raw_cadence_skipped
            << ",\"idle_captures\":" << capture_activity.idle_captures
            << ",\"publication_sequence_gaps\":" << publication_sequence_gaps
            << ",\"capture_sequence_gaps\":" << capture_stats.pairer.left_sequence_gaps + capture_stats.pairer.right_sequence_gaps
            << ",\"outstanding_leases\":" << capture->outstanding_leases()
            << ",\"frame_completion_errors\":"
            << capture_stats.frame_completion_errors
            << ",\"trigger_errors\":" << capture_stats.trigger_errors
            << ",\"trigger_attempts\":" << capture_stats.trigger_attempts
            << ",\"trigger_deadline_overruns\":"
            << capture_stats.trigger_deadline_overruns
            << ",\"ready_pair_replacements\":"
            << capture_stats.ready_pair_replacements
            << ",\"request_requeue_failures\":"
            << capture_stats.request_requeue_failures
            << ",\"scheduler_accepted\":" << scheduler_stats.accepted
            << ",\"scheduler_pending_replaced\":"
            << scheduler_stats.pending_replaced
            << ",\"scheduler_protected_pending_dropped\":" << scheduler_stats.protected_pending_dropped
            << ",\"scheduler_processed\":" << scheduler_stats.processed
            << ",\"scheduler_processing_failures\":"
            << scheduler_stats.processing_failures
            << ",\"scheduler_storage_exhausted_dropped\":"
            << scheduler_stats.storage_exhausted_dropped
            << ",\"scheduler_completed_dropped\":"
            << scheduler_stats.completed_dropped
            << ",\"cpu_dmabuf_mappings_started\":"
            << mapping_stats.mappings_started
            << ",\"cpu_dmabuf_mapping_cache_hits\":"
            << mapping_stats.mapping_cache_hits
            << ",\"cpu_dmabuf_cached_mappings\":"
            << mapping_stats.cached_mappings
            << ",\"cpu_dmabuf_cached_bytes\":"
            << mapping_stats.cached_bytes
            << ",\"cpu_dmabuf_sync_start_failures\":"
            << mapping_stats.sync_start_failures
            << ",\"cpu_dmabuf_sync_end_failures\":"
            << mapping_stats.sync_end_failures
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
            << ",\"stream_mode\":"
            << json_string(depthd::encoding::name(encoder_config.mode))
            << ",\"encoded_codec\":"
            << json_string(depthd::encoding::name(encoder_config.codec))
            << ",\"encoded_submitted\":" << encoder_stats.submitted
            << ",\"encoded_cadence_limited\":"
            << encoder_stats.cadence_limited
            << ",\"encoding_capture_preflights\":"
            << encoder_stats.capture_preflights
            << ",\"encoding_capture_cadence_limited\":"
            << encoder_stats.capture_cadence_limited
            << ",\"encoding_color_preview_requests\":"
            << encoder_stats.color_preview_requests
            << ",\"encoding_stale_discarded\":"
            << encoder_stats.stale_discarded
            << ",\"encoding_preview_unavailable\":"
            << encoder_stats.preview_unavailable
            << ",\"encoding_image_admitted\":" << encoder_stats.image_admitted
            << ",\"encoding_image_copies\":" << encoder_stats.image_copies
            << ",\"encoding_image_pool_dropped\":" << encoder_stats.image_pool_dropped
            << ",\"encoding_image_rectified\":" << encoder_stats.image_rectified
            << ",\"encoded_pending_replaced\":"
            << encoder_stats.pending_replaced
            << ",\"encoded_frames\":" << encoder_stats.encoded
            << ",\"encode_failures\":" << encoder_stats.failures
            << ",\"encoding_configuration_reloads\":"
            << encoder_stats.configuration_reloads
            << ",\"encoding_configuration_errors\":"
            << encoder_stats.configuration_errors
            << ",\"encoded_bytes\":" << encoder_stats.encoded_bytes
            << ",\"last_encoded_frame_bytes\":"
            << encoder_stats.last_frame_bytes
            << ",\"last_encode_ms\":" << encoder_stats.last_encode_ms
#endif
            << "}\n";
  return 0;
}
#endif

depthd::CaptureOpenOptions capture_options(
    const depthd::config::EffectiveConfig& config) {
  depthd::CaptureOpenOptions result;
  result.left_camera_index = config.capture.left_camera_index;
  result.right_camera_index = config.capture.right_camera_index;
  result.width = config.capture.width;
  result.height = config.capture.height;
  result.pixel_format = config.capture.main_pixel_format;
  result.orientation_degrees = config.capture.orientation_degrees;
  result.buffer_count = config.capture.buffer_count;
  result.target_hz = config.capture.target_hz;
  result.pair_tolerance_ns =
      static_cast<std::uint64_t>(config.capture.hardware_trigger.pair_tolerance_us) *
      1'000U;
  result.hardware_trigger_enabled = config.capture.hardware_trigger.enabled;
  result.trigger_driver_mode = config.capture.hardware_trigger.driver_mode;
  result.gpiochip = config.capture.hardware_trigger.gpiochip;
  result.gpio_line = config.capture.hardware_trigger.line;
  result.trigger_idle_high = config.capture.hardware_trigger.idle_level == "high";
  result.trigger_pulse_width_us =
      config.capture.hardware_trigger.pulse_width_us;
  result.startup_priming_pulses =
      config.capture.hardware_trigger.startup_priming_pulses;
  result.exposure_time_us = config.capture.exposure_time_us;
  result.analogue_gain = config.capture.analogue_gain;
  result.colour_gains = {config.capture.colour_gains[0],
                         config.capture.colour_gains[1]};
  depthd::validate(result);
  return result;
}

void write_identity_json(std::ostream& output,
                         const depthd::CameraIdentity& identity) {
  output << "{\"stable_id\":" << json_string(identity.stable_id)
         << ",\"model\":" << json_string(identity.model)
         << ",\"location\":" << json_string(identity.location)
         << ",\"discovery_index\":" << identity.discovery_index << '}';
}

void write_stream_json(std::ostream& output,
                       const depthd::NegotiatedCameraStream& stream) {
  output << "{\"role\":"
         << json_string(stream.binding.role == depthd::CameraRole::kLogicalLeft
                            ? "left"
                            : "right")
         << ",\"camera\":";
  write_identity_json(output, stream.binding.camera);
  output << ",\"width\":" << stream.grid.width
         << ",\"height\":" << stream.grid.height
         << ",\"pixel_format\":" << json_string(stream.pixel_format)
         << ",\"stride_bytes\":" << stream.stride_bytes
         << ",\"buffer_count\":" << stream.buffer_count << '}';
}

void write_pairer_stats_json(std::ostream& output,
                             const depthd::PairerStats& stats) {
  output << "{\"left_submitted\":" << stats.left_submitted
         << ",\"right_submitted\":" << stats.right_submitted
         << ",\"paired\":" << stats.paired
         << ",\"zero_timestamp_rejected\":"
         << stats.zero_timestamp_rejected
         << ",\"wrong_clock_rejected\":" << stats.wrong_clock_rejected
         << ",\"identity_rejected\":" << stats.identity_rejected
         << ",\"epoch_rejected\":" << stats.epoch_rejected
         << ",\"duplicate_sequence_rejected\":"
         << stats.duplicate_sequence_rejected
         << ",\"nonmonotonic_rejected\":" << stats.nonmonotonic_rejected
         << ",\"stale_dropped\":" << stats.stale_dropped
         << ",\"capacity_dropped\":" << stats.capacity_dropped
         << ",\"maximum_sensor_delta_ns\":"
         << stats.maximum_sensor_delta_ns
         << ",\"left_sequence_gaps\":" << stats.left_sequence_gaps
         << ",\"right_sequence_gaps\":" << stats.right_sequence_gaps << '}';
}

int run_capture_check(const CaptureCheckArguments& arguments) {
  const auto loaded = depthd::config::load(arguments.config_path);
  auto source = depthd::make_pi5_libcamera_capture_source();
  if (!source) {
    std::cerr << "depthd: Pi capture backend "
              << depthd::pi5_capture_backend_status() << '\n';
    return kHardwareError;
  }
  auto options = capture_options(loaded.effective);
  options.allow_userspace_trigger = arguments.allow_userspace_trigger;
  const auto cameras = source->enumerate();

  struct CycleResult {
    depthd::CaptureSessionInfo session;
    std::uint64_t first_pair_sequence{};
    std::uint64_t last_pair_sequence{};
    std::int64_t first_timestamp_ns{};
    std::int64_t last_timestamp_ns{};
    std::uint64_t maximum_consumed_delta_ns{};
    depthd::CaptureRuntimeStats stats;
    std::size_t outstanding_after_stop{};
  };
  std::vector<CycleResult> cycles;
  cycles.reserve(arguments.cycles);
  std::uint64_t previous_epoch = 0U;

  for (std::uint32_t cycle = 0; cycle < arguments.cycles; ++cycle) {
    CycleResult result;
    result.session = source->start(options);
    if (result.session.capture_epoch <= previous_epoch) {
      source->stop();
      throw std::runtime_error("capture epoch did not advance after restart");
    }
    previous_epoch = result.session.capture_epoch;
    const double expected_seconds =
        static_cast<double>(arguments.pairs) / options.target_hz;
    const auto cycle_budget = std::chrono::seconds(
        std::max<std::int64_t>(30, static_cast<std::int64_t>(expected_seconds * 3.0)));
    const auto deadline = std::chrono::steady_clock::now() + cycle_budget;
    std::uint64_t previous_pair_sequence = 0U;

    for (std::uint32_t count = 0; count < arguments.pairs; ++count) {
      if (std::chrono::steady_clock::now() >= deadline) {
        source->stop();
        throw std::runtime_error("capture-check cycle exceeded its time budget");
      }
      auto pair = source->wait_for_pair(
          std::chrono::milliseconds(arguments.pair_timeout_ms));
      if (!pair) {
        source->stop();
        throw std::runtime_error("timed out waiting for a stereo pair");
      }
      if (pair->capture_epoch != result.session.capture_epoch ||
          pair->pair_sequence <= previous_pair_sequence ||
          pair->left.camera_stable_id !=
              result.session.left.binding.camera.stable_id ||
          pair->right.camera_stable_id !=
              result.session.right.binding.camera.stable_id) {
        source->stop();
        throw std::runtime_error("capture pair identity is not coherent");
      }
      if (count == 0U) {
        result.first_pair_sequence = pair->pair_sequence;
        result.first_timestamp_ns = pair->capture_timestamp_ns;
      }
      previous_pair_sequence = pair->pair_sequence;
      result.last_pair_sequence = pair->pair_sequence;
      result.last_timestamp_ns = pair->capture_timestamp_ns;
      result.maximum_consumed_delta_ns =
          std::max(result.maximum_consumed_delta_ns, pair->sensor_delta_ns);
    }
    source->stop();
    result.stats = source->runtime_stats();
    result.outstanding_after_stop = source->outstanding_leases();
    if (result.outstanding_after_stop != 0U) {
      throw std::runtime_error("camera request lease remained after capture stop");
    }
    if (result.stats.callback_or_trigger_errors != 0U ||
        result.stats.request_requeue_failures != 0U) {
      std::ostringstream diagnostic;
      diagnostic << "{\"callback_or_trigger_errors\":"
                 << result.stats.callback_or_trigger_errors
                 << ",\"frame_completion_errors\":"
                 << result.stats.frame_completion_errors
                 << ",\"trigger_errors\":" << result.stats.trigger_errors
                 << ",\"trigger_attempts\":"
                 << result.stats.trigger_attempts
                 << ",\"trigger_deadline_overruns\":"
                 << result.stats.trigger_deadline_overruns
                 << ",\"ready_pair_replacements\":"
                 << result.stats.ready_pair_replacements
                 << ",\"request_requeue_failures\":"
                 << result.stats.request_requeue_failures << '}';
      throw std::runtime_error(
          "capture backend recorded callback, trigger, or requeue failures: " +
          diagnostic.str());
    }
    const auto& pairer = result.stats.pairer;
    if (pairer.zero_timestamp_rejected != 0U ||
        pairer.wrong_clock_rejected != 0U ||
        pairer.identity_rejected != 0U || pairer.epoch_rejected != 0U ||
        pairer.duplicate_sequence_rejected != 0U ||
        pairer.nonmonotonic_rejected != 0U || pairer.stale_dropped != 0U ||
        pairer.capacity_dropped != 0U) {
      std::ostringstream diagnostic;
      write_pairer_stats_json(diagnostic, pairer);
      throw std::runtime_error(
          "capture pairer detected invalid, missing, or ambiguous events: " +
          diagnostic.str());
    }
    cycles.push_back(std::move(result));
  }

  std::cout << "{\"schema_version\":1,\"status\":\"pass\""
            << ",\"configuration_sha256\":"
            << json_string(loaded.configuration_sha256)
            << ",\"backend\":"
            << json_string(depthd::pi5_capture_backend_status())
            << ",\"requested_pairs_per_cycle\":" << arguments.pairs
            << ",\"requested_cycles\":" << arguments.cycles
            << ",\"enumerated_cameras\":[";
  for (std::size_t index = 0; index < cameras.size(); ++index) {
    if (index != 0U) {
      std::cout << ',';
    }
    write_identity_json(std::cout, cameras[index]);
  }
  std::cout << "],\"cycles\":[";
  for (std::size_t index = 0; index < cycles.size(); ++index) {
    if (index != 0U) {
      std::cout << ',';
    }
    const auto& cycle = cycles[index];
    std::cout << "{\"capture_epoch\":" << cycle.session.capture_epoch
              << ",\"trigger_active\":"
              << (cycle.session.hardware_trigger_active ? "true" : "false")
              << ",\"trigger_source\":"
              << json_string(cycle.session.trigger_source)
              << ",\"left_stream\":";
    write_stream_json(std::cout, cycle.session.left);
    std::cout << ",\"right_stream\":";
    write_stream_json(std::cout, cycle.session.right);
    std::cout << ",\"first_pair_sequence\":" << cycle.first_pair_sequence
              << ",\"last_pair_sequence\":" << cycle.last_pair_sequence
              << ",\"first_timestamp_ns\":" << cycle.first_timestamp_ns
              << ",\"last_timestamp_ns\":" << cycle.last_timestamp_ns
              << ",\"maximum_consumed_delta_ns\":"
              << cycle.maximum_consumed_delta_ns
              << ",\"outstanding_leases_after_stop\":"
              << cycle.outstanding_after_stop << ",\"pairer_stats\":";
    write_pairer_stats_json(std::cout, cycle.stats.pairer);
    std::cout << ",\"callback_or_trigger_errors\":"
              << cycle.stats.callback_or_trigger_errors
              << ",\"frame_completion_errors\":"
              << cycle.stats.frame_completion_errors
              << ",\"trigger_errors\":" << cycle.stats.trigger_errors
              << ",\"trigger_attempts\":"
              << cycle.stats.trigger_attempts
              << ",\"trigger_deadline_overruns\":"
              << cycle.stats.trigger_deadline_overruns
              << ",\"ready_pair_replacements\":"
              << cycle.stats.ready_pair_replacements
              << ",\"request_requeue_failures\":"
              << cycle.stats.request_requeue_failures << '}';
  }
  std::cout << "]}\n";
  return 0;
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc < 2) {
    print_usage(std::cerr);
    return kUsageOrValidationError;
  }

  const std::string_view command(argv[1]);
  try {
#if defined(DEPTHD_WITH_NATIVE_PIPELINE)
    if (command == "--calibration-check") {
      if (argc != 6) throw std::invalid_argument("use --calibration-check CONFIG BUNDLE DEVICE_IDENTITY user|factory");
      const auto loaded = depthd::config::load(argv[2]);
      const std::string_view authority(argv[5]);
      if (authority != "user" && authority != "factory") throw std::invalid_argument("invalid calibration authority");
      const auto calibration = depthd::load_and_prepare_active_calibration(
          argv[3], authority == "factory" ? depthd::CalibrationAuthority::kFactoryValidated : depthd::CalibrationAuthority::kUserValidated,
          loaded.effective.stereo.working_width, loaded.effective.stereo.working_height);
      std::ifstream identity_file(argv[4]);
      std::string unit; std::getline(identity_file, unit);
      if (!identity_file || unit.empty() || unit != calibration.unit_id) throw std::invalid_argument("calibration belongs to another device");
      if (calibration.preset_id != loaded.effective.product.profile_id) throw std::invalid_argument("calibration capture profile does not match configuration");
      if (calibration.image_view.grid.width != loaded.effective.capture.width || calibration.image_view.grid.height != loaded.effective.capture.height)
        throw std::invalid_argument("calibration image geometry does not match capture");
      std::cout << "{\"valid\":true,\"bundle_id\":" << json_string(calibration.bundle_id) << "}\n";
      return 0;
    }
    if (command == "--pipeline-check") {
      return run_pipeline_check(parse_pipeline_arguments(argc, argv, false));
    }
    if (command == "--run") {
      return run_native_daemon(parse_pipeline_arguments(argc, argv, true));
    }
#endif
    if (command == "--capture-check") {
      return run_capture_check(parse_capture_check(argc, argv));
    }
    if (argc != 3) {
      print_usage(std::cerr);
      return kUsageOrValidationError;
    }
    if (command != "--check-config" &&
        command != "--dump-effective-config") {
      std::cerr << "depthd: unknown command '" << command << "'\n";
      print_usage(std::cerr);
      return kUsageOrValidationError;
    }

    const std::filesystem::path path(argv[2]);
    const auto loaded = depthd::config::load(path);
    if (command == "--dump-effective-config") {
      std::cout << depthd::config::dump_json(loaded) << '\n';
      return 0;
    }

    std::cout << "configuration valid: " << path.string() << '\n'
              << "configuration sha256: "
              << loaded.configuration_sha256 << '\n';
    for (const auto& warning : loaded.warnings) {
      std::cout << "warning: " << warning.path << ": " << warning.message
                << '\n';
    }
    return 0;
  } catch (const depthd::config::ConfigError& error) {
    std::cerr << "depthd: "
              << (error.kind() == depthd::config::ConfigErrorKind::kIo
                      ? "configuration I/O error: "
                      : "configuration validation failed: ")
              << error.what() << '\n';
    return error.kind() == depthd::config::ConfigErrorKind::kIo
               ? kIoError
               : kUsageOrValidationError;
  } catch (const std::invalid_argument& error) {
    std::cerr << "depthd: usage or validation error: " << error.what() << '\n';
    return kUsageOrValidationError;
  } catch (const std::exception& error) {
    std::cerr << "depthd: capture/runtime error: " << error.what() << '\n';
    return command == "--capture-check" || command == "--run"
               ? kHardwareError
               : kInternalError;
  }
}
