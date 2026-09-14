#include "depthd/stereo_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <vector>

#include <opencv2/core.hpp>

int main() {
  constexpr int width = 728;
  constexpr int height = 544;
  constexpr int expected_disparity = 24;
  cv::Mat left_storage(height, width + 17, CV_8UC1);
  cv::Mat left = left_storage(cv::Rect(0, 0, width, height));
  cv::RNG random(20260712);
  random.fill(left, cv::RNG::UNIFORM, 0, 256);
  cv::Mat right_storage = cv::Mat::zeros(height, width + 23, CV_8UC1);
  cv::Mat right = right_storage(cv::Rect(0, 0, width, height));
  left(cv::Rect(expected_disparity, 0, width - expected_disparity, height))
      .copyTo(right(cv::Rect(0, 0, width - expected_disparity, height)));

  depthd::CpuSgbmConfig config;
  config.working_min_disparity = 8;
  config.accepted_min_disparity_px = 8.0F;
  config.opencv_threads = 1;

  auto native_backend = depthd::make_cpu_sgbm_native_backend(config);
  const auto native_metadata = native_backend->metadata();
  if (native_backend->name() != std::string_view("opencv_sgbm_cpu") ||
      native_metadata.grid.width != 364U ||
      native_metadata.grid.height != 272U ||
      native_metadata.grid.reference_width != 728U ||
      native_metadata.grid.x_scale_to_reference != 2.0 ||
      native_metadata.accepted_min_disparity_px != 8.0F ||
      native_metadata.accepted_max_disparity_px != 64.0F ||
      native_metadata.algorithmic_quantization_px != 1.0F / 16.0F ||
      native_metadata.confidence_model !=
          depthd::ConfidenceModel::kBinarySgbmSupportV1 ||
      native_metadata.derivation !=
          depthd::DisparityDerivation::kNativeMatcherGrid) {
    return 1;
  }

  const auto native_pixels =
      static_cast<std::size_t>(native_metadata.grid.width) *
      native_metadata.grid.height;
  std::vector<float> native_disparity(
      native_pixels, std::numeric_limits<float>::quiet_NaN());
  std::vector<std::uint8_t> native_confidence(native_pixels);
  std::vector<std::uint8_t> native_validity(native_pixels);
  const depthd::ImageView left_view{
      left.ptr<std::uint8_t>(), width, height, left.step,
      depthd::ImageEncoding::kGray8};
  const depthd::ImageView right_view{
      right.ptr<std::uint8_t>(), width, height, right.step,
      depthd::ImageEncoding::kGray8};
  const depthd::MutableDisparityProductsView native_output{
      {native_disparity.data(), native_metadata.grid,
       native_metadata.grid.width},
      {native_confidence.data(), native_metadata.grid,
       native_metadata.grid.width},
      {native_validity.data(), native_metadata.grid,
       native_metadata.grid.width}};
  depthd::StereoBackendStats native_stats;
  native_backend->process_native(left_view, right_view, native_output,
                                 native_stats);

  std::vector<float> native_central;
  for (std::uint32_t y = 40U; y < native_metadata.grid.height - 40U; ++y) {
    for (std::uint32_t x = 110U; x < native_metadata.grid.width - 40U; ++x) {
      const auto index = static_cast<std::size_t>(y) *
                             native_metadata.grid.width +
                         x;
      const auto valid = native_validity[index] ==
                         static_cast<std::uint8_t>(
                             depthd::ValidityCode::kValid);
      if (valid) {
        if (!std::isfinite(native_disparity[index]) ||
            native_confidence[index] != 255U) {
          return 2;
        }
        native_central.push_back(native_disparity[index]);
      } else if (!std::isnan(native_disparity[index]) ||
                 native_confidence[index] != 0U) {
        return 3;
      }
    }
  }
  if (native_central.empty()) {
    return 4;
  }
  const auto native_middle =
      native_central.begin() +
      static_cast<std::ptrdiff_t>(native_central.size() / 2U);
  std::nth_element(native_central.begin(), native_middle,
                   native_central.end());
  if (std::abs(*native_middle - expected_disparity / 2.0F) > 0.75F ||
      native_stats.valid_fraction < 0.45F ||
      native_stats.searchable_valid_fraction < 0.65F) {
    return 5;
  }
  for (std::uint32_t y = 0; y < native_metadata.grid.height; ++y) {
    for (std::uint32_t x = 0; x < native_metadata.valid_roi.x; ++x) {
      const auto index = static_cast<std::size_t>(y) *
                             native_metadata.grid.width +
                         x;
      if (native_validity[index] != static_cast<std::uint8_t>(
                                        depthd::ValidityCode::kOutsideRoi) ||
          !std::isnan(native_disparity[index])) {
        return 6;
      }
    }
  }

  auto backend = depthd::make_cpu_sgbm_backend(config);
  if (backend->name() != std::string_view("opencv_sgbm_cpu")) {
    return 7;
  }

  std::vector<float> disparity(static_cast<std::size_t>(width * height),
                               std::numeric_limits<float>::quiet_NaN());
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
    return 8;
  }
  const auto middle = central.begin() + static_cast<std::ptrdiff_t>(central.size() / 2U);
  std::nth_element(central.begin(), middle, central.end());
  if (std::abs(*middle - expected_disparity) > 1.0F) {
    return 9;
  }
  if (stats.valid_fraction < 0.50F ||
      stats.searchable_valid_fraction < 0.70F || stats.total_ms <= 0.0 ||
      stats.match_ms <= 0.0) {
    return 10;
  }
  // The runtime control file switches algorithms without changing the native
  // geometry or reconstructing the camera/rectification pipeline.
  const auto control = std::filesystem::temp_directory_path() /
                       "nuvo-stereo-control-test.json";
  const auto write_control = [&](const char* algorithm, const int maximum) {
    std::ofstream stream(control, std::ios::trunc);
    stream << "{\"schema_version\":1,\"algorithm\":\"" << algorithm
           << "\",\"min_disparity\":0,\"search_range\":128,"
              "\"max_disparity\":" << maximum << ",\"block_size\":9,"
              "\"uniqueness_ratio\":1,\"speckle_window_size\":30,"
              "\"speckle_range\":2,\"disparity_12_max_diff\":1,"
              "\"pre_filter_cap\":31,\"bm_pre_filter_size\":9,"
              "\"bm_texture_threshold\":10,\"specular_threshold\":256,"
              "\"opencv_threads\":1}";
    stream.close();
  };
  write_control("bm", 96);
  depthd::CpuSgbmConfig full;
  full.working_width = width;
  full.working_height = height;
  full.working_disparities = 128;
  full.accepted_max_disparity_px = 128.0F;
  full.control_file = control.string();
  auto selectable = depthd::make_cpu_sgbm_native_backend(full);
  if (selectable->name() != "opencv_bm_cpu" ||
      selectable->metadata().grid.width != width ||
      selectable->metadata().grid.height != height ||
      selectable->metadata().confidence_model !=
          depthd::ConfidenceModel::kBinaryBmSupportV1 ||
      selectable->metadata().accepted_max_disparity_px != 96.0F) return 11;
  const auto full_metadata = selectable->metadata();
  std::vector<float> full_disparity(static_cast<std::size_t>(width * height));
  std::vector<std::uint8_t> full_confidence(full_disparity.size());
  std::vector<std::uint8_t> full_validity(full_disparity.size());
  const depthd::MutableDisparityProductsView full_output{
      {full_disparity.data(), full_metadata.grid, width},
      {full_confidence.data(), full_metadata.grid, width},
      {full_validity.data(), full_metadata.grid, width}};
  depthd::StereoBackendStats bm_stats;
  selectable->process_native(left_view, right_view, full_output, bm_stats);
  if (bm_stats.valid_fraction < 0.20F ||
      !std::isfinite(full_disparity[200U * width + 300U]) ||
      std::abs(full_disparity[200U * width + 300U] -
               expected_disparity) > 1.0F) return 12;
  write_control("sgbm", 128);
  const auto observed_time = std::filesystem::last_write_time(control);
  std::filesystem::last_write_time(control, observed_time +
      std::chrono::seconds(1));
  selectable->refresh_configuration();
  if (selectable->name() != "opencv_sgbm_cpu" ||
      selectable->metadata().accepted_max_disparity_px != 128.0F) return 13;
  write_control("bm", 200);
  std::filesystem::last_write_time(control, observed_time +
      std::chrono::seconds(2));
  selectable->refresh_configuration();
  if (selectable->name() != "opencv_sgbm_cpu" ||
      selectable->metadata().accepted_max_disparity_px != 128.0F) return 14;
  std::filesystem::remove(control);
  return 0;
}
