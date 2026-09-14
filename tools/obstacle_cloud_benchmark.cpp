#include "depthd/obstacle_cloud.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::filesystem::path disparity;
  std::filesystem::path cloud_csv;
  int iterations{100};
  int warmup{10};
};

std::string require_value(int& index, const int argc, char** argv) {
  if (++index >= argc) {
    throw std::invalid_argument(std::string("missing value for ") +
                                argv[index - 1]);
  }
  return argv[index];
}

Options parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--disparity") {
      options.disparity = require_value(index, argc, argv);
    } else if (argument == "--cloud-csv") {
      options.cloud_csv = require_value(index, argc, argv);
    } else if (argument == "--iterations") {
      options.iterations = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--warmup") {
      options.warmup = std::stoi(require_value(index, argc, argv));
    } else if (argument == "--help") {
      std::cout << "Usage: depthd_obstacle_cloud_benchmark --disparity FILE "
                   "[--cloud-csv FILE] [--iterations N] [--warmup N]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  if (options.disparity.empty() || options.iterations < 1 ||
      options.warmup < 0) {
    throw std::invalid_argument(
        "--disparity and positive iteration counts are required");
  }
  return options;
}

double percentile(std::vector<double> values, const double quantile) {
  std::sort(values.begin(), values.end());
  const double position = quantile * static_cast<double>(values.size() - 1U);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  const double fraction = position - static_cast<double>(lower);
  return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

std::vector<float> load_disparity(const std::filesystem::path& path) {
  constexpr std::size_t values = 728U * 544U;
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream || stream.tellg() !=
                     static_cast<std::streamoff>(values * sizeof(float))) {
    throw std::runtime_error("expected a 728x544 contiguous float32 file");
  }
  stream.seekg(0);
  std::vector<float> disparity(values);
  stream.read(reinterpret_cast<char*>(disparity.data()),
              static_cast<std::streamsize>(values * sizeof(float)));
  if (!stream) {
    throw std::runtime_error("failed to read disparity file");
  }
  return disparity;
}

depthd::RigidTransform optical_to_base() {
  return {{0.0, 0.0, 1.0, 0.760, -1.0, 0.0, 0.0, 0.050,
           0.0, -1.0, 0.0, 0.180, 0.0, 0.0, 0.0, 1.0}};
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    const std::vector<float> disparity = load_disparity(options.disparity);
    const depthd::DisparityFrameView view{
        disparity.data(), 728, 544, 728, 128, 0, 600, 544, 0.0F, 128.0F};
    const depthd::StereoCameraModel camera;
    const auto transform = optical_to_base();
    depthd::CompactObstacleCloudGenerator generator({});

    depthd::CompactObstacleCloudResult result;
    for (int index = 0; index < options.warmup; ++index) {
      result = generator.generate(view, camera, transform);
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(options.iterations));
    for (int index = 0; index < options.iterations; ++index) {
      const auto started = Clock::now();
      result = generator.generate(view, camera, transform);
      samples.push_back(std::chrono::duration<double, std::milli>(
                            Clock::now() - started)
                            .count());
    }

    if (!options.cloud_csv.empty()) {
      std::ofstream stream(options.cloud_csv);
      stream << "x,y,z\n";
      for (const auto& point : result.points) {
        stream << point.x << ',' << point.y << ',' << point.z << '\n';
      }
    }

    const auto& stats = result.stats;
    std::cout << std::fixed << std::setprecision(3)
              << "{\n"
              << "  \"iterations\": " << options.iterations << ",\n"
              << "  \"compute_ms_p50\": " << percentile(samples, 0.50)
              << ",\n"
              << "  \"compute_ms_p95\": " << percentile(samples, 0.95)
              << ",\n"
              << "  \"compute_ms_max\": " << *std::max_element(samples.begin(), samples.end())
              << ",\n"
              << "  \"sampled_pixels\": " << stats.sampled_pixels << ",\n"
              << "  \"valid_disparities\": " << stats.valid_disparities << ",\n"
              << "  \"accepted_raw_points\": " << stats.accepted_raw_points << ",\n"
              << "  \"accumulated_voxels\": " << stats.accumulated_voxels << ",\n"
              << "  \"supported_voxels\": " << stats.supported_voxels << ",\n"
              << "  \"accepted_clusters\": " << stats.accepted_clusters << ",\n"
              << "  \"output_points\": " << result.points.size() << ",\n"
              << "  \"voxel_capacity_overflow\": "
              << stats.voxel_capacity_overflow << "\n"
              << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "depthd_obstacle_cloud_benchmark: " << error.what() << '\n';
    return 1;
  }
}
