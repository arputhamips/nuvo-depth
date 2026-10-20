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
  explicit CopyRectifier(std::shared_ptr<unsigned> calls = {}) : calls_(std::move(calls)) {}
  [[nodiscard]] std::string_view name() const noexcept override {
    return "test_copy_rectifier";
  }

  void rectify(const depthd::RectificationCamera,
               const depthd::ImageView& input,
               const depthd::MutableImageView& output,
               depthd::RectificationStats& stats) override {
    if (calls_) ++*calls_;
    const auto channels = input.encoding == depthd::ImageEncoding::kGray8 ? 1U : 3U;
    for (std::uint32_t y = 0; y < input.height; ++y) {
      std::copy_n(input.data + static_cast<std::size_t>(y) * input.stride_bytes,
                  input.width * channels,
                  output.data + static_cast<std::size_t>(y) *
                                    output.stride_bytes);
    }
    stats.compute_ms += 0.01;
    stats.total_ms += 0.01;
  }
 private:
  std::shared_ptr<unsigned> calls_;
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
                       const std::atomic<unsigned>* expected_releases = nullptr,
                       std::shared_ptr<std::vector<const std::uint8_t*>>
                           validity_addresses = {},
                       std::shared_ptr<depthd::StereoConfigurationState> controls = {})
      : controls_(std::move(controls)),
        expected_releases_(expected_releases),
        validity_addresses_(std::move(validity_addresses)) {
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

  [[nodiscard]] depthd::StereoConfigurationState configuration_state() const override {
    return controls_ ? *controls_ : depthd::StereoConfigurationState{};
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
    if (validity_addresses_) {
      validity_addresses_->push_back(output.validity.data);
    }
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
    stats.match_ms += 0.05;
  }

 private:
  depthd::DisparityMetadata metadata_{};
  std::shared_ptr<depthd::StereoConfigurationState> controls_;
  const std::atomic<unsigned>* expected_releases_{};
  std::shared_ptr<std::vector<const std::uint8_t*>> validity_addresses_;
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

void test_pool_reuses_storage_without_mutating_retained_frames() {
  auto config = configuration();
  config.frame_pool_capacity = 2U;
  auto scratch_addresses =
      std::make_shared<std::vector<const std::uint8_t*>>();
  auto instance = std::make_unique<depthd::NativeProcessingPipeline>(
      config, std::make_unique<CopyRectifier>(),
      std::make_unique<FixedStereo>(config.matching_calibration.grid, nullptr,
                                    scratch_addresses));
  std::atomic<unsigned> released{};
  auto first = instance->process(request(10U, released));
  auto retained = first;
  const auto* storage_address = first.get();
  const std::array<const void*, 6> product_addresses{
      first->left_rectified.data(), first->right_rectified.data(),
      first->disparity.data(), first->depth_m.data(), first->confidence.data(),
      first->validity.data()};
  auto second = instance->process(request(11U, released));
  require(second && second.get() != storage_address &&
              retained->metadata.frame.capture_sequence == 10U,
          "pool reused or changed a consumer-pinned frame");
  const auto dropped = instance->process(request(12U, released));
  require(!dropped && !instance->has_available_storage() &&
              released.load() == 6U && scratch_addresses->size() == 2U,
          "exhausted pool did not drop the complete input before processing");
  first.reset();
  require(!instance->has_available_storage(),
          "one remaining consumer did not pin its frame slot");
  retained.reset();
  require(instance->has_available_storage(),
          "released consumer did not return its storage slot");
  const auto reused = instance->process(request(13U, released));
  const std::array<const void*, 6> reused_addresses{
      reused->left_rectified.data(), reused->right_rectified.data(),
      reused->disparity.data(), reused->depth_m.data(),
      reused->confidence.data(), reused->validity.data()};
  require(reused.get() == storage_address &&
              reused_addresses == product_addresses &&
              reused->metadata.frame.capture_sequence == 13U &&
              second->metadata.frame.capture_sequence == 11U &&
              reused->stats.rectification.total_ms == 0.02 &&
              reused->stats.stereo.match_ms == 0.05,
          "reused storage reallocated products or retained prior frame state");
  require(scratch_addresses->size() == 3U &&
              (*scratch_addresses)[0] == (*scratch_addresses)[1] &&
              (*scratch_addresses)[0] == (*scratch_addresses)[2],
          "disparity validity scratch was allocated again for each frame");
  instance.reset();
  require(reused->view().metadata.frame.capture_sequence == 13U &&
              second->view().metadata.frame.capture_sequence == 11U,
          "retained frame storage did not outlive its pipeline");
}

void test_pool_clears_optional_preview_products() {
  auto config = configuration();
  config.frame_pool_capacity = 1U;
  auto instance = depthd::NativeProcessingPipeline(
      config, std::make_unique<CopyRectifier>(),
      std::make_unique<FixedStereo>(config.matching_calibration.grid));
  std::atomic<unsigned> released{};
  auto left = request(20U, released);
  left.color_preview = depthd::ColorPreviewRequest::kLeft;
  left.color_preview_yuv420.assign(72U, 19U);
  auto frame = instance.process(std::move(left));
  require(frame->left_color_yuv420.size() == 72U &&
              frame->right_color_yuv420.empty(),
          "left preview was not retained in its frame slot");
  frame.reset();
  auto right = request(21U, released);
  right.color_preview = depthd::ColorPreviewRequest::kRight;
  right.color_preview_yuv420.assign(72U, 29U);
  frame = instance.process(std::move(right));
  require(frame->left_color_yuv420.empty() &&
              frame->right_color_yuv420.size() == 72U &&
              frame->right_color_yuv420.front() == 29U,
          "reused frame retained a stale preview from the other camera");
  frame.reset();
  frame = instance.process(request(22U, released));
  require(frame->left_color_yuv420.empty() &&
              frame->right_color_yuv420.empty(),
          "raw-only frame retained a prior color preview");
}

void test_pool_rejects_unbounded_capacity_and_payload() {
  for (const auto capacity : {0U, 65U}) {
    auto config = configuration();
    config.frame_pool_capacity = capacity;
    bool threw = false;
    try {
      depthd::validate(config);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    require(threw, "invalid frame pool slot bound was accepted");
  }
  auto config = configuration();
  // All fourteen outputs plus previews: 31 image bytes and 10 matching bytes
  // per pixel; unpublished dependency scratch has 11 matching bytes/pixel.
  config.frame_pool_payload_budget_bytes = 8U * (48U * 31U + 12U * 10U) + 12U * 11U;
  depthd::validate(config);
  --config.frame_pool_payload_budget_bytes;
  bool threw = false;
  try {
    depthd::validate(config);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "frame geometry exceeding the total payload bound was accepted");
  config.frame_pool_payload_budget_bytes = 256U * 1024U * 1024U + 1U;
  threw = false;
  try {
    depthd::validate(config);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  require(threw, "frame payload budget exceeded the appliance memory bound");
}

void test_scheduler_recycles_stale_completions_and_drops_pinned_inputs() {
  auto config = configuration();
  config.frame_pool_capacity = 1U;
  auto instance = std::make_unique<depthd::NativeProcessingPipeline>(
      config, std::make_unique<CopyRectifier>(),
      std::make_unique<FixedStereo>(config.matching_calibration.grid));
  depthd::BoundedNativeScheduler scheduler({1U, 2U}, std::move(instance));
  std::atomic<unsigned> released{};
  const auto wait_for = [&](const auto& predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(predicate(), "scheduler did not reach the expected pool state");
  };
  (void)scheduler.try_submit(request(30U, released));
  wait_for([&] { return scheduler.stats().processed == 1U; });
  (void)scheduler.try_submit(request(31U, released));
  wait_for([&] { return scheduler.stats().processed == 2U; });
  const auto newest = scheduler.wait_next(std::chrono::seconds(2));
  require(newest && newest->metadata.frame.capture_sequence == 31U &&
              scheduler.stats().completed_dropped == 1U,
          "pool exhaustion did not recycle the oldest queued completion");
  (void)scheduler.try_submit(request(32U, released));
  wait_for([&] { return scheduler.stats().storage_exhausted_dropped == 1U; });
  require(!scheduler.wait_next(std::chrono::milliseconds(0)) &&
              scheduler.stats().processing_failures == 0U &&
              scheduler.take_last_error().empty() && released.load() == 6U &&
              newest->metadata.frame.capture_sequence == 31U,
          "pinned pool enqueued null output, retained leases, or reported failure");
  scheduler.stop();
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
  auto dmabuf_lease_alive = std::make_shared<std::atomic<bool>>(true);
  auto mapping = std::shared_ptr<const void>(
      new std::uint8_t{0U},
      [mapping_alive](const void* pointer) {
        mapping_alive->store(false);
        delete static_cast<const std::uint8_t*>(pointer);
      });

  depthd::StereoProcessingRequest occupied;
  occupied.left_mapping_owner = mapping;
  occupied.right_mapping_owner = std::move(mapping);
  occupied.left_dmabuf_lease =
      std::shared_ptr<const depthd::DmabufRequestLease>(
          new TestDmabufLease(1U),
          [dmabuf_lease_alive](const depthd::DmabufRequestLease* lease) {
            dmabuf_lease_alive->store(false);
            delete lease;
          });
  occupied.right_dmabuf_lease = occupied.left_dmabuf_lease;
  occupied.dmabuf.emplace();
  occupied.left = {static_cast<const std::uint8_t*>(
                       occupied.left_mapping_owner.get()),
                   1U, 1U, 1U, depthd::ImageEncoding::kGray8};
  occupied.right = occupied.left;
  const auto release = [&] {
    if (mapping_alive->load() || dmabuf_lease_alive->load() ||
        occupied.dmabuf || occupied.left.data || occupied.right.data) {
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
              !mapping_alive->load() && !dmabuf_lease_alive->load(),
          "move assignment released camera leases before dependent views");
}

void test_move_assignment_transfers_preview_and_preserves_self_assignment() {
  for (const auto preview : {depthd::ColorPreviewRequest::kNone,
                            depthd::ColorPreviewRequest::kLeft,
                            depthd::ColorPreviewRequest::kRight}) {
    std::atomic<unsigned> released{};
    auto occupied = request(40U, released);
    occupied.color_preview = depthd::ColorPreviewRequest::kLeft;
    occupied.color_preview_yuv420.assign(72U, 17U);
    auto replacement = request(41U, released);
    replacement.color_preview = preview;
    if (preview != depthd::ColorPreviewRequest::kNone) {
      replacement.color_preview_yuv420.assign(72U, 31U);
    }
    const auto* payload = replacement.color_preview_yuv420.data();
    occupied = std::move(replacement);
    require(released.load() == 2U && occupied.color_preview == preview &&
                replacement.color_preview == depthd::ColorPreviewRequest::kNone &&
                replacement.color_preview_yuv420.empty() &&
                occupied.color_preview_yuv420.size() ==
                    (preview == depthd::ColorPreviewRequest::kNone ? 0U : 72U),
            "move assignment lost the preview or retained the old payload");
    if (preview != depthd::ColorPreviewRequest::kNone) {
      require(occupied.color_preview_yuv420.data() == payload &&
                  occupied.color_preview_yuv420.front() == 31U,
              "move assignment copied or corrupted the optional preview");
    }
    const auto* image = occupied.left.data;
    auto& alias = occupied;
    occupied = std::move(alias);
    require(released.load() == 2U && occupied.left.data == image &&
                occupied.capture.pair_sequence == 41U &&
                occupied.color_preview == preview &&
                occupied.color_preview_yuv420.size() ==
                    (preview == depthd::ColorPreviewRequest::kNone ? 0U : 72U),
            "self-move assignment changed the live request");

    const auto output = pipeline()->process(std::move(occupied));
    const auto& selected = preview == depthd::ColorPreviewRequest::kRight
                               ? output->right_color_yuv420
                               : output->left_color_yuv420;
    require(released.load() == 4U &&
                output->metadata.frame.capture_sequence == 41U &&
                selected.size() ==
                    (preview == depthd::ColorPreviewRequest::kNone ? 0U : 72U) &&
                (selected.empty() || selected.front() == 31U) &&
                (preview == depthd::ColorPreviewRequest::kRight
                     ? output->left_color_yuv420.empty()
                     : output->right_color_yuv420.empty()),
            "moved request did not produce the selected associated preview");
  }
}

void test_rejects_provisional_metric_depth_without_override() {
  auto config = configuration();
  config.frame_pool_capacity = 1U;
  config.image_calibration.authority =
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired;
  config.matching_calibration.authority =
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired;
  auto instance = depthd::NativeProcessingPipeline(
      config, std::make_unique<CopyRectifier>(),
      std::make_unique<FixedStereo>(config.matching_calibration.grid));
  std::atomic<unsigned> released{};
  for (const auto sequence : {9U, 10U}) {
    bool threw = false;
    try {
      (void)instance.process(request(sequence, released));
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    require(threw,
            "provisional depth was authorized or failure pinned its frame slot");
  }
  require(released.load() == 4U,
          "failed processing did not release camera leases");
}

void test_configuration_epoch_tracks_applied_matcher_changes() {
  auto cfg = configuration();
  auto controls = std::make_shared<depthd::StereoConfigurationState>();
  controls->applied_revision = std::string(64U, 'a');
  controls->requested_revision = controls->applied_revision;
  depthd::NativeProcessingPipeline instance(cfg,std::make_unique<CopyRectifier>(),
      std::make_unique<FixedStereo>(cfg.matching_calibration.grid,nullptr,nullptr,controls));
  std::atomic<unsigned> released{};
  const auto first = instance.process(request(1U,released));
  require(first->metadata.frame.configuration_epoch == 7U,"initial configuration epoch changed");
  controls->requested_revision = std::string(64U, 'b');
  controls->rejection_error = "invalid range";
  const auto rejected = instance.process(request(2U,released));
  require(rejected->metadata.frame.configuration_epoch == 7U &&
          rejected->stereo_configuration.applied_revision == std::string(64U, 'a') &&
          rejected->stereo_configuration.rejection_error == "invalid range",
          "rejected controls advanced the epoch or lost active provenance");
  ++controls->generation;
  controls->applied_revision = controls->requested_revision;
  controls->rejection_error.clear();
  const auto applied = instance.process(request(3U,released));
  require(applied->metadata.frame.configuration_epoch == 8U &&
          applied->stereo_configuration.applied_revision == std::string(64U, 'b'),
          "applied matcher generation did not advance processing epoch");
  controls->applied_revision = controls->requested_revision = std::string(64U, 'c');
  const auto equivalent = instance.process(request(4U,released));
  require(equivalent->metadata.frame.configuration_epoch == 8U &&
          equivalent->stereo_configuration.applied_revision == std::string(64U, 'c'),
          "equivalent controls advanced processing epoch");
  require(first->stereo_configuration.applied_revision == std::string(64U, 'a'),
          "retained frame configuration provenance was mutated");
}

void test_demand_computes_only_required_stages_and_products() {
  using namespace depthd::ipc_v2;
  auto cfg = configuration();
  cfg.frame_pool_capacity = 1U;
  auto rectifications = std::make_shared<unsigned>(0U);
  auto matches = std::make_shared<std::vector<const std::uint8_t*>>();
  depthd::NativeProcessingPipeline instance(cfg, std::make_unique<CopyRectifier>(rectifications),
      std::make_unique<FixedStereo>(cfg.matching_calibration.grid, nullptr, matches));
  std::atomic<unsigned> released{};
  for (std::size_t index = 0; index < kProductCount; ++index) {
    const auto kind = static_cast<ProductKind>(index);
    auto input = request(index + 1U, released);
    input.products = product_bit(kind);
    const auto before_rectification = *rectifications;
    const auto before_matching = matches->size();
    const auto output = instance.process(std::move(input));
    require(output && output->available_products == product_bit(kind), "selected output mask changed");
    (void)output->view();
    const bool matcher = (product_bit(kind) & kMatcherProducts) != 0U;
    require(matches->size() - before_matching == (matcher ? 1U : 0U),
            "an image-only selection ran the matcher");
    const bool raw = kind == ProductKind::kLeftMono || kind == ProductKind::kRightMono ||
        kind == ProductKind::kLeftRgb || kind == ProductKind::kRightRgb;
    const bool stereo = kind == ProductKind::kRectifiedStereo || kind == ProductKind::kRectifiedStereoRgb;
    require(*rectifications - before_rectification == (raw ? 0U : matcher || stereo ? 2U : 1U),
            "selection rectified an unwanted camera");
    const bool metric = kind == ProductKind::kDepth || kind == ProductKind::kValidity;
    require(output->stats.depth.pixel_count == (metric ? 12U : 0U),
            "nonmetric selection ran depth conversion");
    require(output->disparity.empty() == (kind != ProductKind::kDisparity) &&
            output->depth_m.empty() == (kind != ProductKind::kDepth) &&
            output->confidence.empty() == (kind != ProductKind::kConfidence) &&
            output->validity.empty() == (kind != ProductKind::kValidity),
            "unrequested matcher products retained output arrays");
    if (kind == ProductKind::kRectifiedStereo) {
      require(output->rectified_stereo.size() == 96U && output->rectified_stereo[0] == 11U &&
              output->rectified_stereo[8] == 22U && output->rectified_stereo[16] == 11U,
              "mono stereo output is not row-wise side by side");
    }
    if (kind == ProductKind::kRectifiedStereoRgb) {
      require(output->rectified_stereo_rgb.size() == 288U &&
              output->rectified_stereo_rgb[0] == 11U && output->rectified_stereo_rgb[24] == 22U,
              "RGB stereo output is not row-wise side by side");
    }
    require(released.load() == 2U * (index + 1U), "image selection retained camera leases");
  }
  // A mono-only subscription is still useful before metric calibration is
  // authorized; it must not accidentally enter the provisional depth gate.
  cfg.image_calibration.authority = cfg.matching_calibration.authority =
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired;
  depthd::NativeProcessingPipeline uncalibrated(cfg, std::make_unique<CopyRectifier>(),
      std::make_unique<FixedStereo>(cfg.matching_calibration.grid));
  auto mono = request(20U, released);
  mono.products = product_bit(ProductKind::kLeftMono);
  require(uncalibrated.process(std::move(mono))->left_mono.size() == 48U,
          "mono-only output entered metric authorization");
}

void test_independent_lanes_share_configuration_epochs() {
  depthd::NativeConfigurationEpochs epochs(1U);
  require(epochs.observe("first", 100) == 1U, "first revision changed initial epoch");
  require(epochs.observe("second", 200) == 2U, "new revision did not advance epoch");
  require(epochs.observe("first", 150) == 1U, "late lane relabelled an old revision");
  require(epochs.observe("third", 300) == 3U, "third revision did not advance epoch");
  require(epochs.observe("third", 400) == 3U, "lane that skipped a revision diverged");
  require(!epochs.observe("unknown-old", 250), "unknown stale revision became current");
  for (int index = 4; index < 24; ++index)
    require(epochs.observe(std::to_string(index), index * 100) == static_cast<unsigned>(index),
            "bounded configuration history lost current epoch");
  require(!epochs.observe("first", 100), "evicted old revision became current");
  require(epochs.observe("first", 2400) == 24U, "reverting controls did not advance epoch");
}

void test_capture_rgb_preserves_channels_and_padded_rows() {
  std::atomic<unsigned> released{};
  auto input = request(1U, released);
  auto pixels = std::make_shared<std::vector<std::uint8_t>>(6U * 27U, 99U);
  for (std::size_t row = 0; row < 6U; ++row) for (std::size_t column = 0; column < 8U; ++column) {
    const auto offset = row * 27U + column * 3U;
    (*pixels)[offset] = 5U; (*pixels)[offset + 1U] = 31U; (*pixels)[offset + 2U] = 207U;
  }
  input.left_mapping_owner = pixels;
  input.left = {pixels->data(), 8U, 6U, 27U, depthd::ImageEncoding::kBgr8};
  std::vector<std::uint8_t> rgb(144U), mono(48U);
  depthd::copy_capture_rgb(input, depthd::CameraRole::kLogicalLeft, rgb);
  depthd::copy_capture_image(input, depthd::CameraRole::kLogicalLeft, false, mono);
  for (std::size_t pixel = 0; pixel < 48U; ++pixel)
    require(rgb[pixel * 3U] == 207U && rgb[pixel * 3U + 1U] == 31U && rgb[pixel * 3U + 2U] == 5U &&
            mono[pixel] == ((77U * 207U + 150U * 31U + 29U * 5U + 128U) >> 8U),
            "capture RGB/mono copy mixed channels or included row padding");
}

}  // namespace

int main() {
  try {
    test_synchronous_pipeline();
    test_independent_lanes_share_configuration_epochs();
    test_configuration_epoch_tracks_applied_matcher_changes();
    test_demand_computes_only_required_stages_and_products();
    test_capture_rgb_preserves_channels_and_padded_rows();
    test_bounded_scheduler();
    test_pool_reuses_storage_without_mutating_retained_frames();
    test_pool_clears_optional_preview_products();
    test_pool_rejects_unbounded_capacity_and_payload();
    test_scheduler_recycles_stale_completions_and_drops_pinned_inputs();
    test_scheduler_replaces_stale_pending_request();
    test_capture_native_dmabuf_path();
    test_move_assignment_releases_mapping_before_camera_lease();
    test_move_assignment_transfers_preview_and_preserves_self_assignment();
    test_rejects_provisional_metric_depth_without_override();
    std::cout << "runtime_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "runtime_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
