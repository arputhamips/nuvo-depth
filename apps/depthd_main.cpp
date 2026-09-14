#include "depthd/capture.hpp"
#include "depthd/config.hpp"

#if defined(DEPTHD_WITH_NATIVE_PIPELINE)
#include "depthd/calibration_store.hpp"
#include "depthd/frame_wire.hpp"
#include "depthd/ipc_v2.hpp"
#include "depthd/runtime_factory.hpp"
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
  std::filesystem::path encoding_config_path{
      "/etc/nuvo-depth/encoding.json"};
  std::filesystem::path stereo_control_path{
      "/etc/nuvo-depth/stereo.json"};
  std::filesystem::path status_path;
  std::filesystem::path trace_directory;
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
            "[--pair-timeout-ms N]\n"
#if defined(DEPTHD_WITH_NATIVE_PIPELINE)
         << "  depthd --pipeline-check PATH --left IMAGE --right IMAGE "
            "--output-dir DIR [--calibration-root DIR] "
            "[--allow-provisional-depth]\n"
         << "  depthd --run PATH [--frames N] [--pair-timeout-ms N] "
            "[--status PATH] [--snapshot-dir DIR] [--calibration-root DIR] "
            "[--calibration-state-root DIR] [--encoding-config PATH] "
            "[--stereo-control PATH] "
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
  for (int index = 3; index < argc; index += 2) {
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(argv[index]) +
                                  " requires a value");
    }
    const std::string_view option(argv[index]);
    const std::string_view value(argv[index + 1]);
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
    } else if (live && option == "--encoding-config") {
      result.encoding_config_path = value;
    } else if (live && option == "--stereo-control") {
      result.stereo_control_path = value;
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
  if (arguments.live && arguments.calibration_root.empty()) {
    const auto calibrations = arguments.calibration_state_root / "calibrations";
    const auto factory = calibrations / "factory" / "active";
    const auto user = calibrations / "user" / "active";
    if (std::filesystem::exists(factory / "manifest.json")) {
      return depthd::load_and_prepare_active_calibration(
          factory, depthd::CalibrationAuthority::kFactoryValidated,
          configuration.stereo.working_width,
          configuration.stereo.working_height);
    }
    if (std::filesystem::exists(user / "manifest.json")) {
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
                       const std::uint64_t capture_sequence_gaps = 0U) {
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
         << ",\"capture_sequence_gaps\":" << capture_sequence_gaps
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
         << ",\"rectification_ms\":" << frame.stats.rectification.total_ms
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
         << ",\"payload_bytes\":" << frame.payload_bytes() << '}';
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
                  const std::uint64_t capture_sequence_gaps = 0U) {
  if (path.empty()) {
    return;
  }
  std::filesystem::create_directories(path.parent_path());
  const auto temporary = path.string() + ".tmp";
  const auto document =
      frame_json(frame, state, published_frames, capture_sequence_gaps) + "\n";
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
  auto pipeline = depthd::make_native_pipeline(
      loaded.effective, calibration, identity, runtime_options(arguments));
  // One worker owns the stateful backends and one newest pending request is
  // replaceable under overload. Complete outputs are likewise latest-first.
  depthd::BoundedNativeScheduler scheduler({1U, 1U}, std::move(pipeline));
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
  depthd::encoding::Configuration encoder_fallback;
  // Missing or invalid runtime controls fail safe to the original raw stream.
  // The packaged configuration explicitly enables raw plus H.264.
  encoder_fallback.mode = depthd::encoding::StreamMode::kRawOnly;
  encoder_fallback.cadence_hz = std::max(
      1U, static_cast<std::uint32_t>(
              std::lround(loaded.effective.outputs.compressed_image_hz)));
  encoder_fallback.jpeg_quality = loaded.effective.outputs.jpeg_quality;
  depthd::encoding::AsyncFrameEncoder image_encoder(
      arguments.encoding_config_path,
      arguments.snapshot_directory / "latest.nve", encoder_fallback,
      arguments.trace_directory);
#endif
  auto capture = depthd::make_pi5_libcamera_capture_source();
  if (!capture) {
    throw std::runtime_error("Pi capture backend " +
                             std::string(depthd::pi5_capture_backend_status()));
  }
  const auto session = capture->start(capture_options(loaded.effective));
  if ((!calibration.left_camera_id.empty() &&
       calibration.left_camera_id != session.left.binding.camera.stable_id) ||
      (!calibration.right_camera_id.empty() &&
       calibration.right_camera_id != session.right.binding.camera.stable_id)) {
    capture->stop();
    throw std::runtime_error(
        "active calibration does not match the bound logical cameras");
  }
#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
  depthd::ipc_v2::LocalFrameServer local_frames;
#endif
  std::unique_ptr<depthd::ipc_v2::Broker> broker;
  std::shared_ptr<const depthd::FrameSetStorage> last_frame;
  std::uint64_t published{};
  std::uint64_t capture_sequence_gaps{};
  std::uint64_t previous_capture_sequence{};
#if defined(DEPTHD_WITH_SYSTEMD)
  bool ready_notified{};
#endif
  stop_requested.store(false);
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
  try {
    while (!stop_requested.load() &&
           (arguments.frames == 0U || published < arguments.frames)) {
      auto pair = capture->wait_for_pair(
          std::chrono::milliseconds(arguments.pair_timeout_ms));
      if (!pair) {
        throw std::runtime_error("timed out waiting for a stereo pair");
      }
      auto processing = depthd::make_cpu_dmabuf_processing_request(
          std::move(*pair));
      trace_isp_pair(arguments.trace_directory, processing);
      const auto submit = scheduler.try_submit(std::move(processing));
      if (submit == depthd::SubmitResult::kStopped) {
        throw std::runtime_error("native scheduler rejected a capture pair");
      }
      if (submit == depthd::SubmitResult::kQueueFull) {
        continue;
      }
      auto frame = scheduler.wait_next(
          std::chrono::milliseconds(arguments.pair_timeout_ms));
      if (!frame) {
        const auto detail = scheduler.take_last_error();
        throw std::runtime_error(detail.empty()
                                     ? "native processing timed out"
                                     : "native processing failed: " + detail);
      }
      if (!broker) {
        broker = std::make_unique<depthd::ipc_v2::Broker>(
            depthd::ipc_v2::make_pool_configuration(frame->metadata),
            identity.run_uuid);
      }
      if (broker->publish(*frame) != depthd::ipc_v2::PublishResult::kPublished) {
        throw std::runtime_error("IPC broker rejected a processed frame");
      }
      const auto capture_sequence = frame->metadata.frame.capture_sequence;
      if (previous_capture_sequence != 0U &&
          capture_sequence > previous_capture_sequence + 1U) {
        capture_sequence_gaps +=
            capture_sequence - previous_capture_sequence - 1U;
      }
      previous_capture_sequence = capture_sequence;
      ++published;
      trace_depth_frame(arguments.trace_directory, *frame);
#if defined(DEPTHD_WITH_LOCAL_FRAME_SERVER)
      local_frames.publish(frame);
#endif
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
      (void)image_encoder.submit(frame);
      if (depthd::encoding::raw_enabled(image_encoder.configuration())) {
        publish_snapshot(arguments.snapshot_directory, *frame, published);
      }
#else
      publish_snapshot(arguments.snapshot_directory, *frame, published);
#endif
      write_status(arguments.status_path, *frame, "running", published,
                   capture_sequence_gaps);
      last_frame = frame;
#if defined(DEPTHD_WITH_SYSTEMD)
      if (!ready_notified) {
        (void)sd_notify(0, "READY=1\nSTATUS=Streaming associated stereo depth");
        ready_notified = true;
      }
      (void)sd_notify(0, "WATCHDOG=1");
#endif
    }
  } catch (...) {
    // Processing requests retain camera mappings and request leases. Drain the
    // sole processing owner before capture tears down libcamera allocations.
    scheduler.stop();
    capture->stop();
    throw;
  }
  scheduler.stop();
  capture->stop();
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
  if (last_frame) {
    write_status(arguments.status_path, *last_frame, "stopped", published,
                 capture_sequence_gaps);
    write_replay_products(arguments.snapshot_directory, *last_frame);
  }
#if defined(DEPTHD_WITH_SYSTEMD)
  (void)sd_notify(0, "STOPPING=1\nSTATUS=Stopped cleanly");
#endif
  std::cout << "{\"schema_version\":1,\"status\":\"stopped\""
            << ",\"capture_epoch\":" << session.capture_epoch
            << ",\"published_frames\":" << published
            << ",\"capture_sequence_gaps\":" << capture_sequence_gaps
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
            << ",\"scheduler_processed\":" << scheduler_stats.processed
            << ",\"scheduler_processing_failures\":"
            << scheduler_stats.processing_failures
            << ",\"scheduler_completed_dropped\":"
            << scheduler_stats.completed_dropped
            << ",\"cpu_dmabuf_mappings_started\":"
            << mapping_stats.mappings_started
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
         << stats.maximum_sensor_delta_ns << '}';
}

int run_capture_check(const CaptureCheckArguments& arguments) {
  const auto loaded = depthd::config::load(arguments.config_path);
  auto source = depthd::make_pi5_libcamera_capture_source();
  if (!source) {
    std::cerr << "depthd: Pi capture backend "
              << depthd::pi5_capture_backend_status() << '\n';
    return kHardwareError;
  }
  const auto options = capture_options(loaded.effective);
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
