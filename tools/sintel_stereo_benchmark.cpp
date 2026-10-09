#include "depthd/config.hpp"
#include "depthd/stereo_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

struct Options {
  std::filesystem::path dataset_root;
  std::filesystem::path config;
  std::filesystem::path stereo_control;
  std::filesystem::path output_dir;
  std::string pass{"final"};
  std::string sequence{"all"};
  std::size_t max_frames{};
  std::size_t warmup{5U};
  double target_hz{10.0};
  bool run{};
  bool self_test{};
  bool resample_output{};
};

struct FramePaths {
  std::string sequence;
  std::string name;
  std::filesystem::path left;
  std::filesystem::path right;
  std::filesystem::path disparity;
  std::filesystem::path occlusion;
  std::filesystem::path out_of_frame;
};

struct PreparedFrame {
  cv::Mat left;
  cv::Mat right;
  cv::Mat truth;
  cv::Mat eligible;
};

struct FrameScore {
  std::uint64_t eligible{};
  std::uint64_t predicted_valid{};
  std::uint64_t bad_one_including_invalid{};
  std::uint64_t bad_two_including_invalid{};
  std::uint64_t bad_three_including_invalid{};
  double absolute_error_sum{};
  double squared_error_sum{};
};

struct DirectionCheck {
  std::uint64_t samples{};
  double right_x_minus_d_error{};
  double right_x_plus_d_error{};
};

[[nodiscard]] std::string value_after(int& index, const int argc, char** argv) {
  if (++index >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + argv[index - 1]);
  }
  return argv[index];
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--dataset-root") {
      options.dataset_root = value_after(index, argc, argv);
    } else if (argument == "--config") {
      options.config = value_after(index, argc, argv);
    } else if (argument == "--stereo-control") {
      options.stereo_control = value_after(index, argc, argv);
    } else if (argument == "--output-dir") {
      options.output_dir = value_after(index, argc, argv);
    } else if (argument == "--pass") {
      options.pass = value_after(index, argc, argv);
    } else if (argument == "--sequence") {
      options.sequence = value_after(index, argc, argv);
    } else if (argument == "--max-frames") {
      options.max_frames = std::stoull(value_after(index, argc, argv));
    } else if (argument == "--warmup") {
      options.warmup = std::stoull(value_after(index, argc, argv));
    } else if (argument == "--target-hz") {
      options.target_hz = std::stod(value_after(index, argc, argv));
    } else if (argument == "--run") {
      options.run = true;
    } else if (argument == "--self-test") {
      options.self_test = true;
    } else if (argument == "--resample-output") {
      options.resample_output = true;
    } else if (argument == "--help") {
      std::cout << "Usage: depthd_sintel_benchmark --dataset-root DIR "
                   "[--pass clean|final] [--sequence NAME|all] "
                   "[--max-frames N] [--config FILE] [--stereo-control FILE] "
                   "[--warmup N] [--target-hz N] [--run --output-dir DIR] "
                   "[--resample-output] [--self-test]\n"
                   "Without --run, only dataset structure and one sample are verified.\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  if (!options.self_test && (options.dataset_root.empty() ||
      (options.pass != "clean" && options.pass != "final") ||
      !std::isfinite(options.target_hz) || options.target_hz <= 0.0)) {
    throw std::invalid_argument("dataset root, pass, or target rate is invalid");
  }
  if (options.run && (options.config.empty() || options.output_dir.empty())) {
    throw std::invalid_argument("--run requires --config and --output-dir");
  }
  return options;
}

[[nodiscard]] std::filesystem::path training_root(
    const std::filesystem::path& root, const std::string& pass) {
  if (std::filesystem::is_directory(root / (pass + "_left"))) return root;
  if (std::filesystem::is_directory(root / "training" / (pass + "_left"))) {
    return root / "training";
  }
  throw std::runtime_error("cannot find Sintel training/" + pass + "_left");
}

[[nodiscard]] std::vector<FramePaths> collect_frames(const Options& options) {
  const auto root = training_root(options.dataset_root, options.pass);
  const auto left_root = root / (options.pass + "_left");
  const auto right_root = root / (options.pass + "_right");
  std::vector<FramePaths> result;
  for (const auto& scene : std::filesystem::directory_iterator(left_root)) {
    if (!scene.is_directory()) continue;
    const auto scene_name = scene.path().filename().string();
    if (options.sequence != "all" && options.sequence != scene_name) continue;
    for (const auto& entry : std::filesystem::directory_iterator(scene.path())) {
      if (!entry.is_regular_file() || entry.path().extension() != ".png") continue;
      const auto name = entry.path().filename().string();
      FramePaths paths{scene_name, name, entry.path(),
                       right_root / scene_name / name,
                       root / "disparities" / scene_name / name,
                       root / "occlusions" / scene_name / name,
                       root / "outofframe" / scene_name / name};
      for (const auto& path : {paths.right, paths.disparity,
                               paths.occlusion, paths.out_of_frame}) {
        if (!std::filesystem::is_regular_file(path)) {
          throw std::runtime_error("missing Sintel pair/label: " + path.string());
        }
      }
      result.push_back(std::move(paths));
    }
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return std::tie(a.sequence, a.name) < std::tie(b.sequence, b.name);
  });
  if (result.empty()) throw std::runtime_error("no complete Sintel frames found");
  if (options.max_frames != 0U && result.size() > options.max_frames) {
    // Spread a bounded matrix run over every sequence instead of taking only
    // the alphabetically first scene. Full runs still retain every frame.
    std::vector<FramePaths> sampled;
    sampled.reserve(options.max_frames);
    for (std::size_t index = 0; index < options.max_frames; ++index) {
      const auto source = options.max_frames == 1U
                              ? 0U
                              : index * (result.size() - 1U) /
                                    (options.max_frames - 1U);
      sampled.push_back(result[source]);
    }
    result = std::move(sampled);
  }
  return result;
}

[[nodiscard]] cv::Mat read_color(const std::filesystem::path& path) {
  auto image = cv::imread(path.string(), cv::IMREAD_COLOR);
  if (image.empty() || image.type() != CV_8UC3) {
    throw std::runtime_error("cannot decode RGB PNG: " + path.string());
  }
  return image;
}

[[nodiscard]] cv::Mat read_mask(const std::filesystem::path& path) {
  auto image = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
  if (image.empty() || image.type() != CV_8UC1) {
    throw std::runtime_error("cannot decode mask PNG: " + path.string());
  }
  return image;
}

[[nodiscard]] float decode_disparity(const cv::Vec3b& bgr) {
  return static_cast<float>(bgr[2]) * 4.0F +
         static_cast<float>(bgr[1]) / 64.0F +
         static_cast<float>(bgr[0]) / 16384.0F;
}

[[nodiscard]] PreparedFrame prepare(const FramePaths& paths,
                                    const int output_width,
                                    const int output_height) {
  const auto left_color = read_color(paths.left);
  const auto right_color = read_color(paths.right);
  const auto disparity_rgb = read_color(paths.disparity);
  const auto occlusion = read_mask(paths.occlusion);
  const auto out_of_frame = read_mask(paths.out_of_frame);
  for (const auto& image : {right_color, disparity_rgb, occlusion, out_of_frame}) {
    if (image.size() != left_color.size()) {
      throw std::runtime_error("Sintel image/label geometry disagrees: " +
                               paths.left.string());
    }
  }

  // Preserve pixel aspect ratio: center-crop the wide Sintel frame, then
  // resize the same crop in both eyes. The crop and ground-truth mask are
  // recorded in the benchmark protocol; this is not native Sintel scoring.
  const int crop_width = std::min(
      left_color.cols,
      static_cast<int>(std::lround(static_cast<double>(left_color.rows) *
                                   output_width / output_height)));
  if (crop_width < 1) throw std::runtime_error("invalid Sintel crop width");
  const int crop_x = (left_color.cols - crop_width) / 2;
  const cv::Rect crop(crop_x, 0, crop_width, left_color.rows);
  const cv::Size output_size(output_width, output_height);
  const double x_scale = static_cast<double>(output_width) / crop_width;

  PreparedFrame frame;
  cv::Mat left_gray;
  cv::Mat right_gray;
  cv::cvtColor(left_color(crop), left_gray, cv::COLOR_BGR2GRAY);
  cv::cvtColor(right_color(crop), right_gray, cv::COLOR_BGR2GRAY);
  cv::resize(left_gray, frame.left, output_size, 0.0, 0.0, cv::INTER_LINEAR);
  cv::resize(right_gray, frame.right, output_size, 0.0, 0.0, cv::INTER_LINEAR);

  cv::Mat truth_source(left_color.size(), CV_32FC1);
  cv::Mat eligible_source(left_color.size(), CV_8UC1);
  for (int y = 0; y < left_color.rows; ++y) {
    const auto* encoded = disparity_rgb.ptr<cv::Vec3b>(y);
    const auto* occluded = occlusion.ptr<std::uint8_t>(y);
    const auto* outside = out_of_frame.ptr<std::uint8_t>(y);
    auto* truth = truth_source.ptr<float>(y);
    auto* eligible = eligible_source.ptr<std::uint8_t>(y);
    for (int x = 0; x < left_color.cols; ++x) {
      // Official Sintel SDK packs disparity magnitude into RGB channels.
      // OpenCV loads BGR, so red is channel 2. Cast before arithmetic.
      const float disparity = decode_disparity(encoded[x]);
      truth[x] = disparity;
      eligible[x] = static_cast<std::uint8_t>(
          occluded[x] == 0U && outside[x] == 0U && disparity > 0.0F &&
          static_cast<double>(x) - disparity >= crop_x);
    }
  }
  cv::resize(truth_source(crop), frame.truth, output_size,
             0.0, 0.0, cv::INTER_NEAREST);
  frame.truth *= x_scale;
  cv::resize(eligible_source(crop), frame.eligible, output_size,
             0.0, 0.0, cv::INTER_NEAREST);
  return frame;
}

[[nodiscard]] depthd::CpuSgbmConfig matcher_config(
    const depthd::config::EffectiveConfig& effective,
    const std::filesystem::path& control) {
  const auto& profile = effective.stereo;
  if (profile.backend != "cpu_sgbm" ||
      effective.capture.width != profile.output_width ||
      effective.capture.height != profile.output_height) {
    throw std::runtime_error("benchmark requires a CPU SGBM product profile");
  }
  depthd::CpuSgbmConfig result;
  result.control_file = control.string();
  result.input_width = profile.output_width;
  result.input_height = profile.output_height;
  result.working_width = profile.working_width;
  result.working_height = profile.working_height;
  const double scale = static_cast<double>(profile.output_width) /
                       profile.working_width;
  result.working_min_disparity = static_cast<std::int32_t>(
      std::floor(static_cast<double>(profile.min_disparity) / scale));
  result.accepted_min_disparity_px =
      static_cast<float>(static_cast<double>(profile.min_disparity) / scale);
  result.accepted_max_disparity_px =
      static_cast<float>(static_cast<double>(profile.max_disparity) / scale);
  result.working_disparities = static_cast<int>(profile.working_disparities);
  result.block_size = static_cast<int>(profile.block_size);
  result.uniqueness_ratio = static_cast<int>(profile.uniqueness_ratio);
  result.speckle_window_size = static_cast<int>(profile.speckle_window_size);
  result.speckle_range = static_cast<int>(profile.speckle_range);
  result.disparity_12_max_diff = profile.disparity_12_max_diff;
  result.pre_filter_cap = static_cast<int>(profile.pre_filter_cap);
  result.opencv_threads = static_cast<int>(profile.opencv_threads);
  return result;
}

[[nodiscard]] depthd::ImageView view_of(const cv::Mat& image) {
  return {image.ptr<std::uint8_t>(), static_cast<std::uint32_t>(image.cols),
          static_cast<std::uint32_t>(image.rows), image.step,
          depthd::ImageEncoding::kGray8};
}

[[nodiscard]] DirectionCheck check_disparity_direction(
    const PreparedFrame& frame) {
  DirectionCheck result;
  for (int y = 4; y < frame.left.rows - 4; y += 4) {
    const auto* left = frame.left.ptr<std::uint8_t>(y);
    const auto* right = frame.right.ptr<std::uint8_t>(y);
    const auto* truth = frame.truth.ptr<float>(y);
    const auto* eligible = frame.eligible.ptr<std::uint8_t>(y);
    for (int x = 128; x < frame.left.cols - 4; x += 4) {
      const float d = truth[x];
      if (eligible[x] == 0U || d <= 0.0F || d >= 128.0F) continue;
      const auto offset = static_cast<int>(std::lround(d));
      if (x - offset < 0 || x + offset >= frame.left.cols) continue;
      ++result.samples;
      result.right_x_minus_d_error +=
          std::abs(static_cast<int>(left[x]) - static_cast<int>(right[x - offset]));
      result.right_x_plus_d_error +=
          std::abs(static_cast<int>(left[x]) - static_cast<int>(right[x + offset]));
    }
  }
  return result;
}

[[nodiscard]] FrameScore score_frame(const PreparedFrame& frame,
                                     const depthd::DisparityMetadata& metadata,
                                     const std::vector<float>& prediction,
                                     const std::vector<std::uint8_t>& validity) {
  if (metadata.grid.width != static_cast<std::uint32_t>(frame.truth.cols) ||
      metadata.grid.height != static_cast<std::uint32_t>(frame.truth.rows)) {
    throw std::runtime_error("native matcher grid differs from Nuvo geometry");
  }
  FrameScore result;
  const auto& roi = metadata.valid_roi;
  for (std::uint32_t y = roi.y; y < roi.y + roi.height; ++y) {
    const auto* truth = frame.truth.ptr<float>(static_cast<int>(y));
    const auto* eligible = frame.eligible.ptr<std::uint8_t>(static_cast<int>(y));
    for (std::uint32_t x = roi.x; x < roi.x + roi.width; ++x) {
      const float expected = truth[x];
      if (eligible[x] == 0U || !std::isfinite(expected) ||
          expected < metadata.accepted_min_disparity_px ||
          expected >= metadata.accepted_max_disparity_px) continue;
      ++result.eligible;
      const auto index = static_cast<std::size_t>(y) * metadata.grid.width + x;
      const bool valid = validity[index] ==
                         static_cast<std::uint8_t>(depthd::ValidityCode::kValid) &&
                         std::isfinite(prediction[index]);
      if (!valid) {
        ++result.bad_one_including_invalid;
        ++result.bad_two_including_invalid;
        ++result.bad_three_including_invalid;
        continue;
      }
      ++result.predicted_valid;
      const auto error = std::abs(prediction[index] - expected);
      result.absolute_error_sum += error;
      result.squared_error_sum += static_cast<double>(error) * error;
      if (error > 1.0F) ++result.bad_one_including_invalid;
      if (error > 2.0F) ++result.bad_two_including_invalid;
      if (error > 3.0F) ++result.bad_three_including_invalid;
    }
  }
  return result;
}

int self_test() {
  if (std::abs(decode_disparity(cv::Vec3b(0U, 64U, 5U)) - 21.0F) >
          1e-6F ||
      std::abs(decode_disparity(cv::Vec3b(0U, 0U, 100U)) - 400.0F) >
          1e-6F) {
    throw std::runtime_error("Sintel RGB disparity decoder failed self-test");
  }
  PreparedFrame frame;
  frame.truth = cv::Mat(2, 4, CV_32FC1, cv::Scalar(4.0F));
  frame.eligible = cv::Mat(2, 4, CV_8UC1, cv::Scalar(1));
  depthd::DisparityMetadata metadata;
  metadata.grid = depthd::make_reference_grid(4U, 2U);
  metadata.valid_roi = {1U, 0U, 3U, 2U};
  metadata.accepted_min_disparity_px = 0.0F;
  metadata.accepted_max_disparity_px = 128.0F;
  std::vector<float> prediction(8U, 4.0F);
  prediction[5] = 10.0F;
  std::vector<std::uint8_t> validity(
      8U, static_cast<std::uint8_t>(depthd::ValidityCode::kValid));
  validity[6] = static_cast<std::uint8_t>(depthd::ValidityCode::kInconsistent);
  const auto score = score_frame(frame, metadata, prediction, validity);
  if (score.eligible != 6U || score.predicted_valid != 5U ||
      score.bad_one_including_invalid != 2U ||
      score.bad_two_including_invalid != 2U ||
      score.bad_three_including_invalid != 2U ||
      std::abs(score.absolute_error_sum - 6.0) > 1e-6) {
    throw std::runtime_error("Sintel score accounting failed self-test");
  }
  std::cout << "Sintel benchmark self-test: pass (no dataset or matcher run)\n";
  return 0;
}

[[nodiscard]] double percentile(std::vector<double> values, const double q) {
  if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(values.begin(), values.end());
  const auto position = q * static_cast<double>(values.size() - 1U);
  const auto lo = static_cast<std::size_t>(std::floor(position));
  const auto hi = static_cast<std::size_t>(std::ceil(position));
  return values[lo] + (values[hi] - values[lo]) *
                          (position - static_cast<double>(lo));
}

void add_score(FrameScore& total, const FrameScore& part) {
  total.eligible += part.eligible;
  total.predicted_valid += part.predicted_valid;
  total.bad_one_including_invalid += part.bad_one_including_invalid;
  total.bad_two_including_invalid += part.bad_two_including_invalid;
  total.bad_three_including_invalid += part.bad_three_including_invalid;
  total.absolute_error_sum += part.absolute_error_sum;
  total.squared_error_sum += part.squared_error_sum;
}

[[nodiscard]] double ratio(const std::uint64_t numerator,
                           const std::uint64_t denominator) {
  return denominator > 0U ? static_cast<double>(numerator) /
                                static_cast<double>(denominator)
                          : std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] double ratio(const double numerator,
                           const std::uint64_t denominator) {
  return denominator > 0U ? numerator / static_cast<double>(denominator)
                          : std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] std::string control_digest(const std::filesystem::path& path) {
  if (path.empty()) return "";
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open stereo control file");
  const std::string bytes(std::istreambuf_iterator<char>{input}, {});
  if (input.bad()) throw std::runtime_error("cannot read stereo control file");
  return depthd::config::sha256_hex(bytes);
}

int run(const Options& options, const std::vector<FramePaths>& frames) {
  if (std::filesystem::exists(options.output_dir) &&
      !std::filesystem::is_empty(options.output_dir)) {
    throw std::invalid_argument("output directory must be empty");
  }
  const auto loaded = depthd::config::load(options.config);
  const auto control_sha256 = control_digest(options.stereo_control);
  const auto config = matcher_config(loaded.effective, options.stereo_control);
  auto metadata_probe = depthd::make_cpu_sgbm_native_backend(config);
  const auto native_metadata = metadata_probe->metadata();
  const bool resampled = native_metadata.grid.width != config.input_width ||
                         native_metadata.grid.height != config.input_height;
  if (resampled && !options.resample_output) {
    throw std::runtime_error(
        "non-native matcher grid requires --resample-output");
  }
  const auto input_grid =
      depthd::make_reference_grid(config.input_width, config.input_height);
  const auto metadata = resampled
                            ? depthd::make_resampled_disparity_metadata(
                                  native_metadata, input_grid)
                            : native_metadata;
  auto native_matcher = resampled
                            ? std::unique_ptr<depthd::NativeDisparityBackend>{}
                            : depthd::make_cpu_sgbm_native_backend(config);
  auto legacy_matcher = resampled ? depthd::make_cpu_sgbm_backend(config)
                                  : std::unique_ptr<depthd::StereoBackend>{};
  const auto pixels = static_cast<std::size_t>(metadata.grid.width) *
                      metadata.grid.height;
  std::vector<float> disparity(pixels);
  std::vector<std::uint8_t> confidence(pixels);
  std::vector<std::uint8_t> validity(pixels);
  const depthd::MutableDisparityProductsView output{
      {disparity.data(), metadata.grid, metadata.grid.width},
      {confidence.data(), metadata.grid, metadata.grid.width},
      {validity.data(), metadata.grid, metadata.grid.width}};
  const auto process = [&](const PreparedFrame& frame,
                           depthd::StereoBackendStats& stats) {
    if (resampled) {
      const depthd::MutableDisparityView legacy_output{
          disparity.data(), metadata.grid.width, metadata.grid.height,
          metadata.grid.width};
      legacy_matcher->process(view_of(frame.left), view_of(frame.right),
                              legacy_output, stats);
      std::transform(disparity.begin(), disparity.end(), validity.begin(),
                     [](const float value) {
                       return static_cast<std::uint8_t>(
                           std::isfinite(value)
                               ? depthd::ValidityCode::kValid
                               : depthd::ValidityCode::kInconsistent);
                     });
    } else {
      native_matcher->process_native(view_of(frame.left), view_of(frame.right),
                                     output, stats);
    }
  };
  for (std::size_t index = 0; index < std::min(options.warmup, frames.size()); ++index) {
    const auto frame = prepare(frames[index], static_cast<int>(config.input_width),
                               static_cast<int>(config.input_height));
    depthd::StereoBackendStats ignored;
    process(frame, ignored);
  }

  std::filesystem::create_directories(options.output_dir);
  std::ofstream per_frame(options.output_dir / "frames.csv");
  if (!per_frame) throw std::runtime_error("cannot create per-frame report");
  per_frame << "sequence,frame,eligible,predicted_valid,coverage,"
               "bad_1_including_invalid,bad_2_including_invalid,"
               "bad_3_including_invalid,mae_valid_only,rmse_valid_only,"
               "preprocess_ms,match_ms,postprocess_ms,total_ms\n";
  FrameScore aggregate;
  std::vector<double> latency;
  std::size_t over_budget_frames = 0U;
  const double budget_ms = 1000.0 / options.target_hz;
  for (const auto& paths : frames) {
    const auto frame = prepare(paths, static_cast<int>(config.input_width),
                               static_cast<int>(config.input_height));
    depthd::StereoBackendStats stats;
    process(frame, stats);
    const auto score = score_frame(frame, metadata, disparity, validity);
    add_score(aggregate, score);
    latency.push_back(stats.total_ms);
    if (stats.total_ms > budget_ms) ++over_budget_frames;
    per_frame << paths.sequence << ',' << paths.name << ',' << score.eligible
              << ',' << score.predicted_valid << ','
              << ratio(score.predicted_valid, score.eligible) << ','
              << ratio(score.bad_one_including_invalid, score.eligible) << ','
              << ratio(score.bad_two_including_invalid, score.eligible) << ','
              << ratio(score.bad_three_including_invalid, score.eligible) << ','
              << ratio(score.absolute_error_sum, score.predicted_valid) << ','
              << std::sqrt(ratio(score.squared_error_sum,
                                 score.predicted_valid)) << ','
              << stats.preprocess_ms << ',' << stats.match_ms << ','
              << stats.postprocess_ms << ',' << stats.total_ms << '\n';
  }
  if (!per_frame) throw std::runtime_error("cannot write per-frame report");
  if (aggregate.eligible == 0U || aggregate.predicted_valid == 0U) {
    throw std::runtime_error("no eligible valid predictions to report");
  }
  if (control_digest(options.stereo_control) != control_sha256) {
    throw std::runtime_error("stereo control file changed during the benchmark");
  }
  std::ofstream summary(options.output_dir / "summary.json");
  if (!summary) throw std::runtime_error("cannot create summary report");
  summary << std::fixed << std::setprecision(6)
          << "{\n  \"dataset\": \"MPI Sintel stereo training\",\n"
          << "  \"pass\": \"" << options.pass << "\",\n"
          << "  \"sequence\": \"" << options.sequence << "\",\n"
          << "  \"config_sha256\": \"" << loaded.configuration_sha256 << "\",\n"
          << "  \"stereo_control_sha256\": \""
          << control_sha256 << "\",\n"
          << "  \"backend\": \""
          << (resampled ? legacy_matcher->name() : native_matcher->name())
          << "\",\n"
          << "  \"input_width\": " << config.input_width << ",\n"
          << "  \"input_height\": " << config.input_height << ",\n"
          << "  \"matcher_width\": " << native_metadata.grid.width << ",\n"
          << "  \"matcher_height\": " << native_metadata.grid.height << ",\n"
          << "  \"output_width\": " << metadata.grid.width << ",\n"
          << "  \"output_height\": " << metadata.grid.height << ",\n"
          << "  \"resampled_output\": " << (resampled ? "true" : "false")
          << ",\n"
          << "  \"max_disparity_exclusive_px\": "
          << metadata.accepted_max_disparity_px << ",\n"
          << "  \"frame_count\": " << frames.size() << ",\n"
          << "  \"eligible_pixels\": " << aggregate.eligible << ",\n"
          << "  \"valid_prediction_pixels\": " << aggregate.predicted_valid << ",\n"
          << "  \"coverage\": " << ratio(aggregate.predicted_valid, aggregate.eligible)
          << ",\n  \"bad_1_including_invalid\": "
          << ratio(aggregate.bad_one_including_invalid, aggregate.eligible)
          << ",\n  \"bad_2_including_invalid\": "
          << ratio(aggregate.bad_two_including_invalid, aggregate.eligible)
          << ",\n  \"bad_3_including_invalid\": "
          << ratio(aggregate.bad_three_including_invalid, aggregate.eligible)
          << ",\n  \"mae_valid_only_px\": "
          << ratio(aggregate.absolute_error_sum, aggregate.predicted_valid)
          << ",\n  \"rmse_valid_only_px\": "
          << std::sqrt(ratio(aggregate.squared_error_sum,
                             aggregate.predicted_valid))
          << ",\n  \"target_hz\": " << options.target_hz
          << ",\n  \"matcher_over_budget_frames\": " << over_budget_frames
          << ",\n  \"matcher_ms_p50\": " << percentile(latency, 0.5)
          << ",\n  \"matcher_ms_p95\": " << percentile(latency, 0.95)
          << "\n}\n";
  if (!summary) throw std::runtime_error("cannot write summary report");
  std::cout << "Sintel benchmark complete: " << frames.size()
            << " frames; results in " << options.output_dir << '\n';
  return 0;
}

}  // namespace

int main(const int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    if (options.self_test) return self_test();
    const auto frames = collect_frames(options);
    const auto& sample = frames.front();
    const auto left = read_color(sample.left);
    const auto right = read_color(sample.right);
    if (left.size() != right.size()) {
      throw std::runtime_error("sample left/right geometry differs");
    }
    std::cout << "Sintel " << options.pass << ": " << frames.size()
              << " complete frames; first=" << sample.sequence << '/'
              << sample.name << "; source=" << left.cols << 'x' << left.rows
              << '\n';
    if (!options.run) {
      const auto prepared = prepare(sample, 728, 544);
      const auto direction = check_disparity_direction(prepared);
      std::cout << "Preflight only: decoded disparity and masks; prepared="
                << prepared.left.cols << 'x' << prepared.left.rows
                << "; direction samples=" << direction.samples
                << "; |left-right(x-d)|="
                << ratio(direction.right_x_minus_d_error, direction.samples)
                << "; |left-right(x+d)|="
                << ratio(direction.right_x_plus_d_error, direction.samples)
                << "; no matcher run.\n";
      if (direction.samples == 0U ||
          direction.right_x_minus_d_error >= direction.right_x_plus_d_error) {
        throw std::runtime_error("Sintel disparity direction check failed");
      }
      if (!options.config.empty()) {
        const auto loaded = depthd::config::load(options.config);
        const auto config = matcher_config(loaded.effective, options.stereo_control);
        const auto matcher = depthd::make_cpu_sgbm_native_backend(config);
        const auto native = matcher->metadata().grid;
        if (config.input_width != 728U || config.input_height != 544U ||
            ((!options.resample_output) &&
             (native.width != config.input_width ||
              native.height != config.input_height))) {
          throw std::runtime_error(
              "benchmark profile/output geometry is not authorized");
        }
        std::cout << "Matcher configuration verified: " << matcher->name()
                  << ", " << native.width << 'x' << native.height
                  << (options.resample_output ? " -> 728x544" : " native")
                  << ", config sha256="
                  << loaded.configuration_sha256 << '\n';
      }
      return 0;
    }
    return run(options, frames);
  } catch (const std::exception& error) {
    std::cerr << "depthd_sintel_benchmark: " << error.what() << '\n';
    return 1;
  }
}
