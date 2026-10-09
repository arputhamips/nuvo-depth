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
#include <utility>
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

// Keep the actual OpenCV backend in the integration path while checking the
// camera requests have already been requeued before matching starts.
class ReleasedCaptureBackend final : public depthd::NativeDisparityBackend {
 public:
  ReleasedCaptureBackend(
      std::unique_ptr<depthd::NativeDisparityBackend> backend,
      const std::atomic<unsigned>& active_leases)
      : backend_(std::move(backend)), active_leases_(active_leases) {}

  void refresh_configuration() override { backend_->refresh_configuration(); }
  depthd::StereoConfigurationState configuration_state() const override {
    return backend_->configuration_state();
  }
  std::string_view name() const noexcept override { return backend_->name(); }
  depthd::DisparityMetadata metadata() const noexcept override {
    return backend_->metadata();
  }
  void process_native(const depthd::ImageView& left,
                      const depthd::ImageView& right,
                      const depthd::MutableDisparityProductsView& output,
                      depthd::StereoBackendStats& stats) override {
    require(active_leases_.load() == 0U,
            "real matcher started before capture leases were released");
    backend_->process_native(left, right, output, stats);
  }

 private:
  std::unique_ptr<depthd::NativeDisparityBackend> backend_;
  const std::atomic<unsigned>& active_leases_;
};

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

    auto color_left =
        std::make_shared<std::vector<std::uint8_t>>(width * height * 3U);
    auto color_right =
        std::make_shared<std::vector<std::uint8_t>>(width * height * 3U);
    for (std::size_t index = 0; index < left->size(); ++index) {
      for (const auto& pair : {std::pair{left, color_left},
                               std::pair{right, color_right}}) {
        (*pair.second)[index * 3U] = (*pair.first)[index];
        (*pair.second)[index * 3U + 1U] =
            static_cast<std::uint8_t>((*pair.first)[index] / 2U + 19U);
        (*pair.second)[index * 3U + 2U] =
            static_cast<std::uint8_t>(255U - (*pair.first)[index]);
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
    pipeline_config.frame_pool_capacity = 1U;

    std::atomic<unsigned> releases{};
    std::atomic<unsigned> active_leases{};
    const auto make_request = [&](
        const std::uint64_t sequence,
        const depthd::ImageEncoding encoding = depthd::ImageEncoding::kGray8) {
      depthd::StereoProcessingRequest request;
      request.capture.capture_epoch = 1U;
      request.capture.pair_sequence = sequence;
      request.capture.capture_timestamp_ns =
          static_cast<std::int64_t>(sequence) * 1000;
      request.capture.left.role = depthd::CameraRole::kLogicalLeft;
      request.capture.left.camera_stable_id = "left";
      request.capture.left.capture_epoch = 1U;
      request.capture.left.stream_sequence = sequence;
      request.capture.left.sensor_timestamp_ns =
          request.capture.capture_timestamp_ns;
      request.capture.left.sensor_clock = depthd::CaptureClock::kBoottime;
      request.capture.left.grid = image_grid;
      ++active_leases;
      request.capture.left.lease =
          depthd::CaptureBufferLease(1U, 1U, [&] {
            ++releases;
            --active_leases;
          });
      request.capture.right.role = depthd::CameraRole::kLogicalRight;
      request.capture.right.camera_stable_id = "right";
      request.capture.right.capture_epoch = 1U;
      request.capture.right.stream_sequence = sequence;
      request.capture.right.sensor_timestamp_ns =
          request.capture.capture_timestamp_ns + 10;
      request.capture.right.sensor_clock = depthd::CaptureClock::kBoottime;
      request.capture.right.grid = image_grid;
      ++active_leases;
      request.capture.right.lease =
          depthd::CaptureBufferLease(1U, 2U, [&] {
            ++releases;
            --active_leases;
          });
      const auto& source_left = encoding == depthd::ImageEncoding::kGray8
                                    ? left : color_left;
      const auto& source_right = encoding == depthd::ImageEncoding::kGray8
                                     ? right : color_right;
      const auto channels = encoding == depthd::ImageEncoding::kGray8 ? 1U : 3U;
      request.left_mapping_owner = source_left;
      request.right_mapping_owner = source_right;
      request.left = {source_left->data(), width, height, width * channels,
                      encoding};
      request.right = {source_right->data(), width, height, width * channels,
                       encoding};
      return request;
    };

    depthd::NativeProcessingPipeline pipeline(
        pipeline_config, std::move(rectifier),
        std::make_unique<ReleasedCaptureBackend>(std::move(stereo), active_leases));
    auto frame = pipeline.process(make_request(1U));
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
    const auto* storage_address = frame.get();
    const std::array<const void*, 6> product_addresses{
        frame->left_rectified.data(), frame->right_rectified.data(),
        frame->disparity.data(), frame->depth_m.data(), frame->confidence.data(),
        frame->validity.data()};
    const auto confidence = frame->confidence;
    const auto validity = frame->validity;
    require(!pipeline.process(make_request(2U)) && releases.load() == 4U &&
                frame->metadata.frame.capture_sequence == 1U,
            "real processing did not drop input when its only slot was pinned");
    for (std::uint64_t sequence = 3U; sequence < 19U; ++sequence) {
      frame.reset();
      frame = pipeline.process(make_request(sequence));
      const std::array<const void*, 6> current_addresses{
          frame->left_rectified.data(), frame->right_rectified.data(),
          frame->disparity.data(), frame->depth_m.data(),
          frame->confidence.data(), frame->validity.data()};
      require(frame.get() == storage_address &&
                  current_addresses == product_addresses &&
                  frame->view().metadata.frame.capture_sequence == sequence &&
                  frame->confidence == confidence && frame->validity == validity,
              "repeated real processing reallocated or corrupted pooled products");
    }
    require(releases.load() == 36U,
            "repeated pooled integration processing leaked capture leases");
    std::uint64_t sequence = 19U;
    for (const auto& [encoding, format] :
         {std::pair{depthd::ImageEncoding::kBgr8,
                    depthd::ImageProductFormat::kBgr8},
          std::pair{depthd::ImageEncoding::kRgb8,
                    depthd::ImageProductFormat::kRgb8}}) {
      frame.reset();
      frame = pipeline.process(make_request(sequence++, encoding));
      const auto view = frame->view();
      require(view.left_rectified.format == format &&
                  view.right_rectified.format == format &&
                  view.left_rectified.stride_bytes == width * 3U &&
                  view.right_rectified.stride_bytes == width * 3U &&
                  frame->left_rectified == *color_left &&
                  frame->right_rectified == *color_right &&
                  frame.get() == storage_address &&
                  frame->left_rectified.data() == product_addresses[0] &&
                  frame->right_rectified.data() == product_addresses[1],
              "real color pipeline lost format, color bytes, or pooled storage");
    }
    frame.reset();
    frame = pipeline.process(make_request(sequence));
    require(frame->view().left_rectified.format ==
                depthd::ImageProductFormat::kGray8 &&
                frame->left_rectified == *left &&
                frame->right_rectified == *right && releases.load() == 42U &&
                active_leases.load() == 0U,
            "color-to-gray reuse corrupted images or leaked capture leases");
    std::cout << "runtime_opencv_integration_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "runtime_opencv_integration_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
