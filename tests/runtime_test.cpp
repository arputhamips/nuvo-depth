#include "depthd/runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

class CopyRectifier final : public depthd::RectificationBackend {
 public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_copy_rectifier";
  }

  void rectify(const depthd::RectificationCamera,
               const depthd::ImageView& input,
               const depthd::MutableImageView& output,
               depthd::RectificationStats& stats) override {
    for (std::uint32_t y = 0; y < input.height; ++y) {
      std::copy_n(input.data + static_cast<std::size_t>(y) * input.stride_bytes,
                  input.width,
                  output.data + static_cast<std::size_t>(y) *
                                    output.stride_bytes);
    }
    stats.compute_ms += 0.01;
    stats.total_ms += 0.01;
  }
};

class BlockingCopyRectifier final : public depthd::RectificationBackend {
 public:
  BlockingCopyRectifier(std::shared_ptr<std::mutex> mutex,
                        std::shared_ptr<std::condition_variable> condition,
                        std::shared_ptr<bool> started,
                        std::shared_ptr<bool> released)
      : mutex_(std::move(mutex)),
        condition_(std::move(condition)),
        started_(std::move(started)),
        released_(std::move(released)) {}

  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_blocking_copy_rectifier";
  }

  void rectify(const depthd::RectificationCamera camera,
               const depthd::ImageView& input,
               const depthd::MutableImageView& output,
               depthd::RectificationStats& stats) override {
    if (camera == depthd::RectificationCamera::kLeft) {
      std::unique_lock lock(*mutex_);
      *started_ = true;
      condition_->notify_all();
      condition_->wait(lock, [&] { return *released_; });
    }
    for (std::uint32_t y = 0; y < input.height; ++y) {
      std::copy_n(input.data + static_cast<std::size_t>(y) * input.stride_bytes,
                  input.width,
                  output.data + static_cast<std::size_t>(y) *
                                    output.stride_bytes);
    }
    stats.compute_ms += 0.01;
    stats.total_ms += 0.01;
  }

 private:
  std::shared_ptr<std::mutex> mutex_;
  std::shared_ptr<std::condition_variable> condition_;
  std::shared_ptr<bool> started_;
  std::shared_ptr<bool> released_;
};

class TestDmabufLease final : public depthd::DmabufRequestLease {
 public:
  explicit TestDmabufLease(const std::uint64_t id) : id_(id) {}
  [[nodiscard]] bool valid() const noexcept override { return true; }
  [[nodiscard]] std::uint64_t request_id() const noexcept override {
    return id_;
  }
 private:
  std::uint64_t id_{};
};

class DmabufCopyRectifier final : public depthd::RectificationBackend {
 public:
  explicit DmabufCopyRectifier(std::shared_ptr<bool> used)
      : used_(std::move(used)) {}
  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_dmabuf_rectifier";
  }
  void rectify(depthd::RectificationCamera, const depthd::ImageView&,
               const depthd::MutableImageView&,
               depthd::RectificationStats&) override {
    throw std::runtime_error("CPU rectification was selected unexpectedly");
  }
  [[nodiscard]] bool supports_dmabuf_pair() const noexcept override {
    return true;
  }
  void rectify_dmabuf_pair(
      const depthd::DmabufStereoInput& input,
      const depthd::MutableImageView& left,
      const depthd::MutableImageView& right,
      depthd::RectificationStats& stats) override {
    require(input.left.lease != nullptr && input.right.lease != nullptr &&
                input.left.lease->valid() && input.right.lease->valid(),
            "DMA-BUF request leases were not retained");
    for (std::uint32_t row = 0; row < left.height; ++row) {
      std::copy_n(input.left_cpu_fallback.data + row *
                      input.left_cpu_fallback.stride_bytes,
                  left.width, left.data + row * left.stride_bytes);
      std::copy_n(input.right_cpu_fallback.data + row *
                      input.right_cpu_fallback.stride_bytes,
                  right.width, right.data + row * right.stride_bytes);
    }
    *used_ = true;
    stats.compute_ms = 0.01;
  }
 private:
  std::shared_ptr<bool> used_;
};

class FixedStereo final : public depthd::NativeDisparityBackend {
 public:
  explicit FixedStereo(depthd::PixelGrid grid,
                       const std::atomic<unsigned>* expected_releases = nullptr)
      : expected_releases_(expected_releases) {
    metadata_.grid = grid;
    metadata_.valid_roi = {0U, 0U, grid.width, grid.height};
    metadata_.accepted_min_disparity_px = 0.0F;
    metadata_.accepted_max_disparity_px = 32.0F;
    metadata_.algorithmic_quantization_px = 1.0F / 16.0F;
    metadata_.confidence_model =
        depthd::ConfidenceModel::kBinarySgbmSupportV1;
  }

  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_fixed_stereo";
  }

  [[nodiscard]] depthd::DisparityMetadata metadata() const noexcept override {
    return metadata_;
  }

  void process_native(const depthd::ImageView& left,
                      const depthd::ImageView& right,
                      const depthd::MutableDisparityProductsView& output,
                      depthd::StereoBackendStats& stats) override {
    if (expected_releases_ != nullptr) {
      require(expected_releases_->load() == 2U,
              "camera leases were retained into disparity processing");
    }
    require(left.data[0] == 11U && right.data[0] == 22U,
            "rectified pixels were not routed to stereo");
    for (std::uint32_t y = 0; y < metadata_.grid.height; ++y) {
      for (std::uint32_t x = 0; x < metadata_.grid.width; ++x) {
        const auto index = static_cast<std::size_t>(y) *
                               output.disparity.stride_elements +
                           x;
        output.disparity.data[index] = 10.0F;
        output.confidence.data[index] = 255U;
        output.validity.data[index] =
            static_cast<std::uint8_t>(depthd::ValidityCode::kValid);
      }
    }
    stats.valid_fraction = 1.0F;
    stats.searchable_valid_fraction = 1.0F;
  }

 private:
  depthd::DisparityMetadata metadata_{};
  const std::atomic<unsigned>* expected_releases_{};
};

depthd::RectifiedCalibrationView calibration(
    const depthd::PixelGrid& grid,
    const depthd::CalibrationIdentity& identity) {
  const std::array<double, 12> left{
      100.0, 0.0, 3.5, 0.0, 0.0, 100.0,
      2.5,   0.0, 0.0, 0.0, 1.0, 0.0};
  const std::array<double, 12> right{
      100.0, 0.0, 3.5, -10.0, 0.0, 100.0,
      2.5,   0.0, 0.0, 0.0,  1.0, 0.0};
  return depthd::make_rectified_calibration(
      identity, depthd::CalibrationAuthority::kFactoryValidated, grid, left,
      right);
}

depthd::NativePipelineConfiguration configuration() {
  depthd::NativePipelineConfiguration result;
  result.device_uuid[0] = 1U;
  result.run_uuid[0] = 2U;
  result.configuration_epoch = 7U;
  const auto image_grid = depthd::make_reference_grid(8U, 6U);
  const auto matching_grid =
      depthd::make_resampled_grid(image_grid, 4U, 3U);
  const auto identity = depthd::calibration_identity_from_sha256(
      "1111111111111111111111111111111111111111111111111111111111111111");
  result.image_calibration = calibration(image_grid, identity);
  result.matching_calibration =
      depthd::resample_rectified_calibration(result.image_calibration,
                                             matching_grid);
  result.depth.minimum_depth_m = 0.01F;
  result.depth.maximum_depth_m = 10.0F;
  return result;
}

depthd::StereoProcessingRequest request(const std::uint64_t sequence,
                                        std::atomic<unsigned>& released) {
  auto left = std::make_shared<std::vector<std::uint8_t>>(48U, 11U);
  auto right = std::make_shared<std::vector<std::uint8_t>>(48U, 22U);
  depthd::StereoProcessingRequest result;
  result.capture.capture_epoch = 3U;
  result.capture.pair_sequence = sequence;
  result.capture.capture_timestamp_ns =
      1'000'000 + static_cast<std::int64_t>(sequence);
  result.capture.left.role = depthd::CameraRole::kLogicalLeft;
  result.capture.left.camera_stable_id = "left-id";
  result.capture.left.capture_epoch = 3U;
  result.capture.left.stream_sequence = sequence;
  result.capture.left.sensor_timestamp_ns = result.capture.capture_timestamp_ns;
  result.capture.left.sensor_clock = depthd::CaptureClock::kBoottime;
  result.capture.left.grid = depthd::make_reference_grid(8U, 6U);
  result.capture.left.pixel_format = "YUV420";
  result.capture.left.lease = depthd::CaptureBufferLease(
      3U, sequence * 2U, [&released] { ++released; });
  result.capture.right.role = depthd::CameraRole::kLogicalRight;
  result.capture.right.camera_stable_id = "right-id";
  result.capture.right.capture_epoch = 3U;
  result.capture.right.stream_sequence = sequence;
  result.capture.right.sensor_timestamp_ns =
      result.capture.capture_timestamp_ns + 10;
  result.capture.right.sensor_clock = depthd::CaptureClock::kBoottime;
  result.capture.right.grid = depthd::make_reference_grid(8U, 6U);
  result.capture.right.pixel_format = "YUV420";
  result.capture.right.lease = depthd::CaptureBufferLease(
      3U, sequence * 2U + 1U, [&released] { ++released; });
  result.left_mapping_owner = left;
  result.right_mapping_owner = right;
  result.left = {left->data(), 8U, 6U, 8U, depthd::ImageEncoding::kGray8};
  result.right = {right->data(), 8U, 6U, 8U,
                  depthd::ImageEncoding::kGray8};
  return result;
}

std::unique_ptr<depthd::NativeProcessingPipeline> pipeline() {
  return std::make_unique<depthd::NativeProcessingPipeline>(
      configuration(), std::make_unique<CopyRectifier>(),
      std::make_unique<FixedStereo>(
          configuration().matching_calibration.grid));
}

void test_synchronous_pipeline() {
  std::atomic<unsigned> released{};
  auto instance = std::make_unique<depthd::NativeProcessingPipeline>(
      configuration(), std::make_unique<CopyRectifier>(),
      std::make_unique<FixedStereo>(configuration().matching_calibration.grid,
                                    &released));
  const auto output = instance->process(request(4U, released));
  require(released.load() == 2U,
          "camera leases were not released after product ownership transfer");
  const auto view = output->view();
  require(view.metadata.frame.capture_epoch == 3U &&
              view.metadata.frame.capture_sequence == 4U &&
              view.metadata.frame.configuration_epoch == 7U,
          "frame identity did not propagate across the native pipeline");
  require(view.left_rectified.grid.width == 8U &&
              view.disparity.grid.width == 4U &&
              view.depth_m.grid.width == 4U,
          "native product geometries are incoherent");
  require(view.left_rectified.data[0] == 11U &&
              view.right_rectified.data[0] == 22U,
          "rectified products were not retained");
  require(std::abs(view.depth_m.data[0] - 0.5F) < 1.0e-5F,
          "metric depth did not use the native-grid calibration");
  require(view.validity.data[0] ==
              static_cast<std::uint8_t>(depthd::ValidityCode::kValid),
          "validity did not remain associated with depth");
  require(output->rectification_backend == "test_copy_rectifier" &&
              output->stereo_backend == "test_fixed_stereo" &&
              output->payload_bytes() == 48U * 2U + 12U * 10U,
          "backend or bounded payload metadata is incorrect");
}

void test_bounded_scheduler() {
  std::atomic<unsigned> released{};
  depthd::BoundedNativeScheduler scheduler({2U, 2U}, pipeline());
  require(scheduler.try_submit(request(1U, released)) ==
              depthd::SubmitResult::kAccepted,
          "scheduler rejected a valid request");
  require(scheduler.try_submit(request(2U, released)) ==
              depthd::SubmitResult::kAccepted,
          "scheduler rejected bounded queued work");
  const auto first = scheduler.wait_next(std::chrono::seconds(2));
  require(first != nullptr, "scheduler did not produce a complete frame set");
  const auto second = scheduler.wait_next(std::chrono::seconds(2));
  require(second != nullptr, "scheduler did not produce the second frame set");
  scheduler.stop();
  const auto stats = scheduler.stats();
  require(!stats.running && stats.accepted == 2U && stats.processed == 2U &&
              stats.processing_failures == 0U,
          "scheduler lifecycle statistics are inconsistent");
  require(released.load() == 4U,
          "scheduler did not release every paired capture lease");
  require(scheduler.try_submit(request(3U, released)) ==
              depthd::SubmitResult::kStopped,
          "stopped scheduler accepted work");
}

void test_scheduler_replaces_stale_pending_request() {
  std::atomic<unsigned> released_leases{};
  auto mutex = std::make_shared<std::mutex>();
  auto condition = std::make_shared<std::condition_variable>();
  auto processing_started = std::make_shared<bool>(false);
  auto allow_processing = std::make_shared<bool>(false);
  auto instance = std::make_unique<depthd::NativeProcessingPipeline>(
      configuration(),
      std::make_unique<BlockingCopyRectifier>(
          mutex, condition, processing_started, allow_processing),
      std::make_unique<FixedStereo>(configuration().matching_calibration.grid));
  // Keep both expected results so this test isolates input freshness.  A
  // completed capacity of one intentionally permits the worker to replace
  // frame 1 with frame 3 before the test thread wakes, which makes the input
  // ordering assertion scheduler-timing dependent under a parallel test run.
  depthd::BoundedNativeScheduler scheduler({1U, 2U}, std::move(instance));

  require(scheduler.try_submit(request(1U, released_leases)) ==
              depthd::SubmitResult::kAccepted,
          "scheduler rejected the in-flight request");
  {
    std::unique_lock lock(*mutex);
    require(condition->wait_for(lock, std::chrono::seconds(2), [&] {
              return *processing_started;
            }),
            "processing worker did not enter the blocking rectifier");
  }
  require(scheduler.try_submit(request(2U, released_leases)) ==
              depthd::SubmitResult::kAccepted &&
              scheduler.try_submit(request(3U, released_leases)) ==
                  depthd::SubmitResult::kAccepted,
          "scheduler did not accept newest pending work");
  require(released_leases.load() == 2U,
          "replaced pending request retained its camera leases");
  {
    std::lock_guard lock(*mutex);
    *allow_processing = true;
  }
  condition->notify_all();

  const auto first = scheduler.wait_next(std::chrono::seconds(2));
  const auto newest = scheduler.wait_next(std::chrono::seconds(2));
  scheduler.stop();
  require(first && newest &&
              first->metadata.frame.capture_sequence == 1U &&
              newest->metadata.frame.capture_sequence == 3U,
          "scheduler did not preserve in-flight plus newest pending order");
  const auto stats = scheduler.stats();
  require(stats.accepted == 3U && stats.pending_replaced == 1U &&
              stats.processed == 2U,
          "scheduler replacement statistics are inconsistent");
  require(released_leases.load() == 6U,
          "freshness scheduler leaked capture leases");
}

void test_capture_native_dmabuf_path() {
  std::atomic<unsigned> released{};
  auto input = request(8U, released);
  input.left_dmabuf_lease = std::make_shared<TestDmabufLease>(16U);
  input.right_dmabuf_lease = std::make_shared<TestDmabufLease>(17U);
  input.dmabuf = depthd::DmabufStereoInput{
      {5, 101U, 8U, 6U, 8U, 0U, depthd::kDmabufModifierInvalid,
       depthd::DmabufPlaneFormat::kR8, input.left_dmabuf_lease.get()},
      {6, 102U, 8U, 6U, 8U, 0U, depthd::kDmabufModifierInvalid,
       depthd::DmabufPlaneFormat::kR8, input.right_dmabuf_lease.get()},
      input.left, input.right, {}};
  auto used = std::make_shared<bool>(false);
  auto instance = depthd::NativeProcessingPipeline(
      configuration(), std::make_unique<DmabufCopyRectifier>(used),
      std::make_unique<FixedStereo>(configuration().matching_calibration.grid));
  const auto output = instance.process(std::move(input));
  require(*used && output->left_rectified.front() == 11U &&
              output->right_rectified.front() == 22U,
          "capture-native DMA-BUF path did not produce associated images");
  require(released.load() == 2U,
          "DMA-BUF processing did not release both camera requests");
}

void test_move_assignment_releases_mapping_before_camera_lease() {
  std::atomic<unsigned> releases{};
  std::atomic<unsigned> ordering_failures{};
  auto mapping_alive = std::make_shared<std::atomic<bool>>(true);
  auto mapping = std::shared_ptr<const void>(
      new std::uint8_t{0U},
      [mapping_alive](const void* pointer) {
        mapping_alive->store(false);
        delete static_cast<const std::uint8_t*>(pointer);
      });

  depthd::StereoProcessingRequest occupied;
  occupied.left_mapping_owner = mapping;
  occupied.right_mapping_owner = std::move(mapping);
  const auto release = [&] {
    if (mapping_alive->load()) {
      ++ordering_failures;
    }
    ++releases;
  };
  occupied.capture.left.lease =
      depthd::CaptureBufferLease(1U, 1U, release);
  occupied.capture.right.lease =
      depthd::CaptureBufferLease(1U, 2U, release);

  depthd::StereoProcessingRequest replacement;
  occupied = std::move(replacement);
  require(releases.load() == 2U && ordering_failures.load() == 0U &&
              !mapping_alive->load(),
          "move assignment released camera leases before mapping owners");
}

void test_rejects_provisional_metric_depth_without_override() {
  auto config = configuration();
  config.image_calibration.authority =
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired;
  config.matching_calibration.authority =
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired;
  auto instance = depthd::NativeProcessingPipeline(
      config, std::make_unique<CopyRectifier>(),
      std::make_unique<FixedStereo>(config.matching_calibration.grid));
  std::atomic<unsigned> released{};
  bool threw = false;
  try {
    (void)instance.process(request(9U, released));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "provisional calibration silently authorized metric depth");
  require(released.load() == 2U,
          "failed processing did not release camera leases");
}

}  // namespace

int main() {
  try {
    test_synchronous_pipeline();
    test_bounded_scheduler();
    test_scheduler_replaces_stale_pending_request();
    test_capture_native_dmabuf_path();
    test_move_assignment_releases_mapping_before_camera_lease();
    test_rejects_provisional_metric_depth_without_override();
    std::cout << "runtime_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "runtime_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
