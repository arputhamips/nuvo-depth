#include "depthd/stereo_backend.hpp"
#include "depthd/contract.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::filesystem::path left;
  std::filesystem::path right;
  std::filesystem::path output_dir;
  int iterations{100};
  int warmup{5};
  int target_hz{10};
  int opencv_threads{1};
  int working_width{364};
  int working_height{272};
  int working_disparities{96};
  int full_disparity_limit{192};
  int disparity_12_max_diff{1};
  int block_size{5};
  int uniqueness_ratio{5};
  int speckle_window_size{50};
  int speckle_range{2};
};

[[nodiscard]] std::string require_value(int& index, const int argc,
                                        char** argv) {
  if (++index >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + argv[index - 1]);
  }
  return argv[index];
}

[[nodiscard]] Options parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--left") {
      options.left = require_value(index, argc, argv);
    } else if (argument == "--right") {
      options.right = require_value(index, argc, argv);
    } else if (argument == "--output-dir") {
      options.output_dir = require_value(index, argc, argv);
    } else if (argument == "--iterations") {
      options.iterations = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--warmup") {
      options.warmup = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--target-hz") {
      options.target_hz = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--opencv-threads") {
      options.opencv_threads = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--working-width") {
      options.working_width = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--working-height") {
      options.working_height = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--working-disparities") {
      options.working_disparities = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--full-disparity-limit") {
      options.full_disparity_limit = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--disp12-max-diff") {
      options.disparity_12_max_diff =
          std::stoi(require_value(index, argc, argv));
    } else if (argument == "--block-size") {
      options.block_size = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--uniqueness-ratio") {
      options.uniqueness_ratio = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--speckle-window-size") {
      options.speckle_window_size = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--speckle-range") {
      options.speckle_range = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--help") {
      std::cout
          << "Usage: depthd_sgbm_benchmark --left FILE --right FILE "
             "[--output-dir DIR] [--iterations N] [--warmup N] "
             "[--target-hz N] [--opencv-threads N] "
             "[--working-width N] [--working-height N] "
             "[--working-disparities N] [--full-disparity-limit N] "
             "[--disp12-max-diff N] "
             "[--block-size N] "
             "[--uniqueness-ratio N] [--speckle-window-size N] "
             "[--speckle-range N]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  if (options.left.empty() || options.right.empty()) {
    throw std::invalid_argument("--left and --right are required");
  }
  if (options.iterations < 1 || options.warmup < 0 || options.target_hz < 1 ||
      options.opencv_threads < 1) {
    throw std::invalid_argument("iteration, rate, and thread counts are invalid");
  }
  if (options.working_disparities < 16 ||
      options.working_disparities % 16 != 0 || options.block_size < 1 ||
      options.block_size % 2 == 0 || options.uniqueness_ratio < 0 ||
      options.speckle_window_size < 0 || options.speckle_range < 0) {
    throw std::invalid_argument("SGBM tuning parameters are invalid");
  }
  if (options.working_width < 1 || options.working_width > 728 ||
      options.working_height < 1 || options.working_height > 544 ||
      options.full_disparity_limit < 1) {
    throw std::invalid_argument("SGBM working geometry or output limit is invalid");
  }
  return options;
}

[[nodiscard]] double percentile(std::vector<double> values,
                                const double quantile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double position = quantile * static_cast<double>(values.size() - 1U);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  const double fraction = position - static_cast<double>(lower);
  return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

[[nodiscard]] depthd::ImageView view_of(const cv::Mat& image) {
  return {image.ptr<std::uint8_t>(), static_cast<std::uint32_t>(image.cols),
          static_cast<std::uint32_t>(image.rows), image.step,
          depthd::ImageEncoding::kBgr8};
}

void save_outputs(const std::filesystem::path& directory, const cv::Mat& left,
                  const cv::Mat& right, const std::vector<float>& disparity,
                  const int full_disparity_limit) {
  if (directory.empty()) {
    return;
  }
  std::filesystem::create_directories(directory);
  cv::imwrite((directory / "left_bgr.png").string(), left);
  cv::imwrite((directory / "right_bgr.png").string(), right);

  cv::Mat disparity_mat(left.rows, left.cols, CV_32FC1,
                        const_cast<float*>(disparity.data()));
  cv::Mat valid = disparity_mat == disparity_mat;
  valid &= disparity_mat > 0.0F;
  cv::Mat normalized = cv::Mat::zeros(disparity_mat.size(), CV_8UC1);
  disparity_mat.convertTo(normalized, CV_8U,
                          255.0 / static_cast<double>(full_disparity_limit));
  cv::Mat color;
  cv::applyColorMap(normalized, color, cv::COLORMAP_TURBO);
  color.setTo(cv::Scalar(0, 0, 0), valid == 0);
  cv::imwrite((directory / "disparity_color.png").string(), color);

  std::ofstream raw(directory / "disparity_float32.bin", std::ios::binary);
  raw.write(reinterpret_cast<const char*>(disparity.data()),
            static_cast<std::streamsize>(disparity.size() * sizeof(float)));

  cv::Mat depth(disparity_mat.size(), CV_32FC1,
                cv::Scalar(std::numeric_limits<float>::quiet_NaN()));
  const cv::Mat depth_valid =
      valid & (disparity_mat < static_cast<float>(full_disparity_limit));
  cv::divide(depthd::kRectifiedFocalLengthPx * depthd::kBaselineM,
             disparity_mat, depth, 1.0, CV_32F);
  depth.setTo(std::numeric_limits<float>::quiet_NaN(), depth_valid == 0);
  std::ofstream depth_raw(directory / "depth_m_float32.bin", std::ios::binary);
  depth_raw.write(reinterpret_cast<const char*>(depth.ptr<float>()),
                  static_cast<std::streamsize>(depth.total() * sizeof(float)));

  const float minimum_depth_m =
      depthd::kRectifiedFocalLengthPx * depthd::kBaselineM /
      static_cast<float>(full_disparity_limit);
  constexpr float maximum_depth_m = 8.0F;
  cv::Mat depth_normalized = cv::Mat::zeros(depth.size(), CV_8UC1);
  for (int row = 0; row < depth.rows; ++row) {
    const float* source = depth.ptr<float>(row);
    std::uint8_t* destination = depth_normalized.ptr<std::uint8_t>(row);
    for (int column = 0; column < depth.cols; ++column) {
      if (std::isfinite(source[column])) {
        const float clipped =
            std::clamp(source[column], minimum_depth_m, maximum_depth_m);
        destination[column] = static_cast<std::uint8_t>(std::lround(
            255.0F * (maximum_depth_m - clipped) /
            (maximum_depth_m - minimum_depth_m)));
      }
    }
  }
  cv::Mat depth_color;
  cv::applyColorMap(depth_normalized, depth_color, cv::COLORMAP_TURBO);
  depth_color.setTo(cv::Scalar(0, 0, 0), depth_valid == 0);
  cv::imwrite((directory / "depth_color.png").string(), depth_color);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    const cv::Mat left = cv::imread(options.left.string(), cv::IMREAD_COLOR);
    const cv::Mat right = cv::imread(options.right.string(), cv::IMREAD_COLOR);
    if (left.empty() || right.empty()) {
      throw std::runtime_error("failed to load the stereo input images");
    }
    if (left.size() != cv::Size(728, 544) || right.size() != left.size()) {
      throw std::runtime_error("expected a 728x544 rectified stereo pair");
    }

    depthd::CpuSgbmConfig config;
    config.opencv_threads = options.opencv_threads;
    config.working_width = static_cast<std::uint32_t>(options.working_width);
    config.working_height = static_cast<std::uint32_t>(options.working_height);
    config.working_disparities = options.working_disparities;
    config.full_resolution_disparities = options.full_disparity_limit;
    config.disparity_12_max_diff = options.disparity_12_max_diff;
    config.block_size = options.block_size;
    config.uniqueness_ratio = options.uniqueness_ratio;
    config.speckle_window_size = options.speckle_window_size;
    config.speckle_range = options.speckle_range;
    auto backend = depthd::make_cpu_sgbm_backend(config);
    std::vector<float> disparity(static_cast<std::size_t>(left.total()),
                                 std::numeric_limits<float>::quiet_NaN());
    const depthd::MutableDisparityView output{
        disparity.data(), static_cast<std::uint32_t>(left.cols),
        static_cast<std::uint32_t>(left.rows),
        static_cast<std::size_t>(left.cols)};

    depthd::StereoBackendStats stats;
    for (int index = 0; index < options.warmup; ++index) {
      backend->process(view_of(left), view_of(right), output, stats);
    }

    std::vector<double> preprocess;
    std::vector<double> match;
    std::vector<double> postprocess;
    std::vector<double> total;
    preprocess.reserve(static_cast<std::size_t>(options.iterations));
    match.reserve(static_cast<std::size_t>(options.iterations));
    postprocess.reserve(static_cast<std::size_t>(options.iterations));
    total.reserve(static_cast<std::size_t>(options.iterations));

    const auto period = std::chrono::duration<double>(1.0 / options.target_hz);
    const auto wall_started = Clock::now();
    auto next_frame = wall_started;
    const std::clock_t cpu_started = std::clock();
    int deadline_misses = 0;
    for (int index = 0; index < options.iterations; ++index) {
      backend->process(view_of(left), view_of(right), output, stats);
      preprocess.push_back(stats.preprocess_ms);
      match.push_back(stats.match_ms);
      postprocess.push_back(stats.postprocess_ms);
      total.push_back(stats.total_ms);

      next_frame += std::chrono::duration_cast<Clock::duration>(period);
      if (Clock::now() > next_frame) {
        ++deadline_misses;
      } else {
        std::this_thread::sleep_until(next_frame);
      }
    }
    const double cpu_seconds =
        static_cast<double>(std::clock() - cpu_started) / CLOCKS_PER_SEC;
    const double wall_seconds =
        std::chrono::duration<double>(Clock::now() - wall_started).count();
    const unsigned int logical_cpus =
        std::max(1U, std::thread::hardware_concurrency());
    const double process_cpu_percent = 100.0 * cpu_seconds / wall_seconds;

    save_outputs(options.output_dir, left, right, disparity,
                 config.full_resolution_disparities);

    std::ostringstream report;
    report << std::fixed << std::setprecision(3)
              << "{\n"
              << "  \"backend\": \"" << backend->name() << "\",\n"
              << "  \"input_width\": " << left.cols << ",\n"
              << "  \"input_height\": " << left.rows << ",\n"
              << "  \"working_width\": " << config.working_width << ",\n"
              << "  \"working_height\": " << config.working_height << ",\n"
              << "  \"full_resolution_disparities\": "
              << config.full_resolution_disparities << ",\n"
              << "  \"working_disparities\": " << config.working_disparities
              << ",\n"
              << "  \"disparity_12_max_diff\": "
              << config.disparity_12_max_diff << ",\n"
              << "  \"block_size\": " << config.block_size << ",\n"
              << "  \"uniqueness_ratio\": " << config.uniqueness_ratio << ",\n"
              << "  \"speckle_window_size\": "
              << config.speckle_window_size << ",\n"
              << "  \"speckle_range\": " << config.speckle_range << ",\n"
              << "  \"opencv_threads\": " << options.opencv_threads << ",\n"
              << "  \"target_hz\": " << options.target_hz << ",\n"
              << "  \"achieved_hz\": " << options.iterations / wall_seconds
              << ",\n"
              << "  \"deadline_misses\": " << deadline_misses << ",\n"
              << "  \"compute_ms_p50\": " << percentile(total, 0.50) << ",\n"
              << "  \"compute_ms_p95\": " << percentile(total, 0.95) << ",\n"
              << "  \"compute_ms_p99\": " << percentile(total, 0.99) << ",\n"
              << "  \"preprocess_ms_p50\": " << percentile(preprocess, 0.50)
              << ",\n"
              << "  \"match_ms_p50\": " << percentile(match, 0.50) << ",\n"
              << "  \"postprocess_ms_p50\": "
              << percentile(postprocess, 0.50) << ",\n"
              << "  \"valid_fraction\": " << stats.valid_fraction << ",\n"
              << "  \"searchable_valid_fraction\": "
              << stats.searchable_valid_fraction << ",\n"
              << "  \"process_cpu_percent_one_core_100\": "
              << process_cpu_percent << ",\n"
              << "  \"soc_cpu_percent_four_core_100\": "
              << process_cpu_percent / logical_cpus << ",\n"
              << "  \"logical_cpus\": " << logical_cpus << "\n"
              << "}\n";
    std::cout << report.str();
    if (!options.output_dir.empty()) {
      std::ofstream metrics(options.output_dir / "metrics.json");
      metrics << report.str();
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "depthd_sgbm_benchmark: " << error.what() << '\n';
    return 1;
  }
}
