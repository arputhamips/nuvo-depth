#include "depthd/opencv_rectification_backend.hpp"
#include "depthd/runtime.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

depthd::RectificationMap identity_map(const std::uint32_t width,
                                      const std::uint32_t height) {
  depthd::RectificationMap result;
  result.width = width;
  result.height = height;
  result.source_x.resize(static_cast<std::size_t>(width) * height);
  result.source_y.resize(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto index = static_cast<std::size_t>(y) * width + x;
      result.source_x[index] = static_cast<float>(x);
      result.source_y[index] = static_cast<float>(y);
    }
  }
  return result;
}

}  // namespace

int main() {
  try {
    constexpr std::uint32_t width = 64U;
    constexpr std::uint32_t height = 48U;
    auto left = std::make_shared<std::vector<std::uint8_t>>(width * height);
    auto right = std::make_shared<std::vector<std::uint8_t>>(width * height);
    std::mt19937 generator(42U);
    for (auto& value : *left) {
      value = static_cast<std::uint8_t>(generator() & 0xffU);
    }
    constexpr std::uint32_t shift = 4U;
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        (*right)[static_cast<std::size_t>(y) * width + x] =
            x + shift < width
                ? (*left)[static_cast<std::size_t>(y) * width + x + shift]
                : 0U;
      }
    }

    depthd::RectificationBackendRegistry registry;
    depthd::register_opencv_rectification_backend(registry);
    const depthd::RectificationBackendConfig rectification_config{
        width, height, width, height};
    const depthd::StereoRectificationMaps maps{identity_map(width, height),
                                                identity_map(width, height)};
    auto rectifier = registry.create("cpu_opencv", "cpu_opencv", false,
                                     rectification_config, maps);
    depthd::CpuSgbmConfig stereo_config;
    stereo_config.input_width = width;
    stereo_config.input_height = height;
    stereo_config.working_width = 32U;
    stereo_config.working_height = 24U;
    stereo_config.working_disparities = 16;
    stereo_config.accepted_max_disparity_px = 16.0F;
    stereo_config.block_size = 5;
    stereo_config.uniqueness_ratio = 0;
    stereo_config.speckle_window_size = 0;
    stereo_config.speckle_range = 0;
    stereo_config.disparity_12_max_diff = 1;
    stereo_config.pre_filter_cap = 31;
    stereo_config.opencv_threads = 1;
    auto stereo = depthd::make_cpu_sgbm_native_backend(stereo_config);

    depthd::NativePipelineConfiguration pipeline_config;
    pipeline_config.device_uuid[0] = 1U;
    pipeline_config.run_uuid[0] = 2U;
    pipeline_config.configuration_epoch = 1U;
    const auto image_grid = depthd::make_reference_grid(width, height);
    const auto matching_grid =
        depthd::make_resampled_grid(image_grid, 32U, 24U);
    const auto identity = depthd::calibration_identity_from_sha256(
        "2222222222222222222222222222222222222222222222222222222222222222");
    const std::array<double, 12> left_projection{
        100.0, 0.0, 31.5, 0.0, 0.0, 100.0,
        23.5,  0.0, 0.0,  0.0, 1.0, 0.0};
    const std::array<double, 12> right_projection{
        100.0, 0.0, 31.5, -10.0, 0.0, 100.0,
        23.5,  0.0, 0.0,   0.0,  1.0, 0.0};
    pipeline_config.image_calibration = depthd::make_rectified_calibration(
        identity, depthd::CalibrationAuthority::kFactoryValidated, image_grid,
        left_projection, right_projection);
    pipeline_config.matching_calibration =
        depthd::resample_rectified_calibration(
            pipeline_config.image_calibration, matching_grid);
    pipeline_config.depth = {0.01F, 100.0F, false};

    std::atomic<unsigned> releases{};
    depthd::StereoProcessingRequest request;
    request.capture.capture_epoch = 1U;
    request.capture.pair_sequence = 1U;
    request.capture.capture_timestamp_ns = 1000;
    request.capture.left.role = depthd::CameraRole::kLogicalLeft;
    request.capture.left.camera_stable_id = "left";
    request.capture.left.capture_epoch = 1U;
    request.capture.left.stream_sequence = 1U;
    request.capture.left.sensor_timestamp_ns = 1000;
    request.capture.left.sensor_clock = depthd::CaptureClock::kBoottime;
    request.capture.left.grid = image_grid;
    request.capture.left.lease =
        depthd::CaptureBufferLease(1U, 1U, [&releases] { ++releases; });
    request.capture.right.role = depthd::CameraRole::kLogicalRight;
    request.capture.right.camera_stable_id = "right";
    request.capture.right.capture_epoch = 1U;
    request.capture.right.stream_sequence = 1U;
    request.capture.right.sensor_timestamp_ns = 1010;
    request.capture.right.sensor_clock = depthd::CaptureClock::kBoottime;
    request.capture.right.grid = image_grid;
    request.capture.right.lease =
        depthd::CaptureBufferLease(1U, 2U, [&releases] { ++releases; });
    request.left_mapping_owner = left;
    request.right_mapping_owner = right;
    request.left = {left->data(), width, height, width,
                    depthd::ImageEncoding::kGray8};
    request.right = {right->data(), width, height, width,
                     depthd::ImageEncoding::kGray8};

    depthd::NativeProcessingPipeline pipeline(
        pipeline_config, std::move(rectifier), std::move(stereo));
    const auto frame = pipeline.process(std::move(request));
    require(frame->rectification_backend == "cpu_opencv" &&
                frame->stereo_backend == "opencv_sgbm_cpu",
            "real processing backends were not selected");
    require(frame->metadata.disparity.grid.width == 32U &&
                frame->metadata.depth.grid.width == 32U,
            "real SGBM native geometry was lost");
    require(frame->stats.stereo.searchable_valid_fraction >= 0.0F &&
                frame->stats.stereo.searchable_valid_fraction <= 1.0F,
            "real SGBM statistics are invalid");
    require(releases.load() == 2U,
            "integration pipeline retained camera requests");
    std::cout << "runtime_opencv_integration_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "runtime_opencv_integration_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
