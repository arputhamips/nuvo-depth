#include "depthd/stereo_backend.hpp"

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
    } else if (argument == "--help") {
      std::cout
          << "Usage: depthd_sgbm_benchmark --left FILE --right FILE "
             "[--output-dir DIR] [--iterations N] [--warmup N] "
             "[--target-hz N] [--opencv-threads N]\n";
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
                  const cv::Mat& right, const std::vector<float>& disparity) {
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
  disparity_mat.convertTo(normalized, CV_8U, 255.0 / 192.0);
  cv::Mat color;
  cv::applyColorMap(normalized, color, cv::COLORMAP_TURBO);
  color.setTo(cv::Scalar(0, 0, 0), valid == 0);
  cv::imwrite((directory / "disparity_color.png").string(), color);

  std::ofstream raw(directory / "disparity_float32.bin", std::ios::binary);
  raw.write(reinterpret_cast<const char*>(disparity.data()),
            static_cast<std::streamsize>(disparity.size() * sizeof(float)));
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

    save_outputs(options.output_dir, left, right, disparity);

    std::cout << std::fixed << std::setprecision(3)
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
              << "  \"process_cpu_percent_one_core_100\": "
              << process_cpu_percent << ",\n"
              << "  \"soc_cpu_percent_four_core_100\": "
              << process_cpu_percent / logical_cpus << ",\n"
              << "  \"logical_cpus\": " << logical_cpus << "\n"
              << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "depthd_sgbm_benchmark: " << error.what() << '\n';
    return 1;
  }
}
