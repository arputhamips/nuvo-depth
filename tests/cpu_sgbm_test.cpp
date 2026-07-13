#include "depthd/stereo_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include <opencv2/core.hpp>

int main() {
  constexpr int width = 728;
  constexpr int height = 544;
  constexpr int expected_disparity = 24;
  cv::Mat left(height, width, CV_8UC1);
  cv::RNG random(20260712);
  random.fill(left, cv::RNG::UNIFORM, 0, 256);
  cv::Mat right = cv::Mat::zeros(left.size(), left.type());
  left(cv::Rect(expected_disparity, 0, width - expected_disparity, height))
      .copyTo(right(cv::Rect(0, 0, width - expected_disparity, height)));

  depthd::CpuSgbmConfig config;
  config.min_disparity = 8;
  config.opencv_threads = 1;
  auto backend = depthd::make_cpu_sgbm_backend(config);
  if (backend->name() != std::string_view("opencv_sgbm_cpu")) {
    return 1;
  }

  std::vector<float> disparity(static_cast<std::size_t>(width * height),
                               std::numeric_limits<float>::quiet_NaN());
  const depthd::ImageView left_view{
      left.ptr<std::uint8_t>(), width, height, left.step,
      depthd::ImageEncoding::kGray8};
  const depthd::ImageView right_view{
      right.ptr<std::uint8_t>(), width, height, right.step,
      depthd::ImageEncoding::kGray8};
  const depthd::MutableDisparityView output{disparity.data(), width, height,
                                            width};
  depthd::StereoBackendStats stats;
  backend->process(left_view, right_view, output, stats);

  std::vector<float> central;
  for (int y = 80; y < height - 80; ++y) {
    for (int x = 220; x < width - 80; ++x) {
      const float value = disparity[static_cast<std::size_t>(y * width + x)];
      if (std::isfinite(value)) {
        central.push_back(value);
      }
    }
  }
  if (central.empty()) {
    return 2;
  }
  const auto middle = central.begin() + static_cast<std::ptrdiff_t>(central.size() / 2U);
  std::nth_element(central.begin(), middle, central.end());
  if (std::abs(*middle - expected_disparity) > 1.0F) {
    return 3;
  }
  if (stats.valid_fraction < 0.50F ||
      stats.searchable_valid_fraction < 0.70F || stats.total_ms <= 0.0 ||
      stats.match_ms <= 0.0) {
    return 4;
  }
  return 0;
}
