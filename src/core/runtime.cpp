#include "depthd/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace depthd {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] bool uuid_present(
    const std::array<std::uint8_t, 16>& value) noexcept {
  return std::any_of(value.begin(), value.end(),
                     [](const std::uint8_t byte) { return byte != 0U; });
}

[[nodiscard]] std::size_t pixel_count(const PixelGrid& grid) {
  validate(grid);
  const auto count = static_cast<std::uint64_t>(grid.width) * grid.height;
  if (count > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error("product grid exceeds addressable memory");
  }
  return static_cast<std::size_t>(count);
}

[[nodiscard]] ImageProductFormat product_format(
    const ImageEncoding encoding) {
  switch (encoding) {
    case ImageEncoding::kGray8:
      return ImageProductFormat::kGray8;
    case ImageEncoding::kBgr8:
      return ImageProductFormat::kBgr8;
    case ImageEncoding::kRgb8:
      return ImageProductFormat::kRgb8;
  }
  throw std::invalid_argument("unsupported processing image encoding");
}

[[nodiscard]] std::size_t channel_count(const ImageEncoding encoding) {
  return encoding == ImageEncoding::kGray8 ? 1U : 3U;
}

[[nodiscard]] std::size_t channel_count(const ImageProductFormat format) {
  return format == ImageProductFormat::kGray8 ? 1U : 3U;
}

[[nodiscard]] double elapsed_ms(const Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

// Rectification copies both camera inputs into frame-owned output storage.
// Drop every mapping/import view before returning the libcamera requests so
// capture can overlap the CPU disparity and depth stages. Keeping the leases
// until process() returned serialized request recycling behind the slowest
// stage and prevented externally-triggered capture from sustaining 60 Hz.
void release_rectified_capture_input(
    StereoProcessingRequest& request) noexcept {
  request.dmabuf.reset();
  request.right_dmabuf_lease.reset();
  request.left_dmabuf_lease.reset();
  request.right = {};
  request.left = {};
  request.right_mapping_owner.reset();
  request.left_mapping_owner.reset();
  request.capture.right.lease.reset();
  request.capture.left.lease.reset();
}

}  // namespace

void validate(const StereoProcessingRequest& request) {
  const auto& pair = request.capture;
  if (pair.capture_epoch == 0U || pair.pair_sequence == 0U ||
      pair.capture_timestamp_ns <= 0) {
    throw std::invalid_argument("processing request has no capture identity");
  }
  if (pair.left.role != CameraRole::kLogicalLeft ||
      pair.right.role != CameraRole::kLogicalRight ||
      pair.left.capture_epoch != pair.capture_epoch ||
      pair.right.capture_epoch != pair.capture_epoch ||
      pair.left.sensor_clock != CaptureClock::kBoottime ||
      pair.right.sensor_clock != CaptureClock::kBoottime) {
    throw std::invalid_argument("processing request stereo identity is incoherent");
  }
  if (!pair.left.lease.active() || !pair.right.lease.active()) {
    throw std::invalid_argument("processing request must retain both capture leases");
  }
  const bool has_cpu_mappings =
      request.left_mapping_owner && request.right_mapping_owner &&
      request.left.data != nullptr && request.right.data != nullptr;
  const bool has_lazy_dmabuf =
      request.dmabuf && request.dmabuf->acquire_cpu_fallback;
  if (!has_cpu_mappings && !has_lazy_dmabuf) {
    throw std::invalid_argument(
        "processing request requires CPU mappings or a lazy DMA-BUF mapper");
  }
  if (request.left.width != pair.left.grid.width ||
      request.left.height != pair.left.grid.height ||
      request.right.width != pair.right.grid.width ||
      request.right.height != pair.right.grid.height ||
      request.left.width != request.right.width ||
      request.left.height != request.right.height ||
      request.left.encoding != request.right.encoding) {
    throw std::invalid_argument("processing image views do not match the pair");
  }
  const auto channels = channel_count(request.left.encoding);
  if (request.left.stride_bytes < request.left.width * channels ||
      request.right.stride_bytes < request.right.width * channels) {
    throw std::invalid_argument("processing image stride is too small");
  }
}

void validate(const NativePipelineConfiguration& configuration) {
  if (!uuid_present(configuration.device_uuid) ||
      !uuid_present(configuration.run_uuid) ||
      configuration.configuration_epoch == 0U) {
    throw std::invalid_argument(
        "native pipeline requires device, run, and configuration identity");
  }
  validate(configuration.image_calibration);
  validate(configuration.matching_calibration);
  if (configuration.image_calibration.identity.sha256 !=
          configuration.matching_calibration.identity.sha256 ||
      configuration.image_calibration.authority !=
          configuration.matching_calibration.authority) {
    throw std::invalid_argument(
        "image and matching calibration must share identity and authority");
  }
  if (!std::isfinite(configuration.depth.minimum_depth_m) ||
      !std::isfinite(configuration.depth.maximum_depth_m) ||
      configuration.depth.minimum_depth_m <= 0.0F ||
      configuration.depth.maximum_depth_m <=
          configuration.depth.minimum_depth_m) {
    throw std::invalid_argument("native pipeline depth limits are invalid");
  }
}

ProductSetView FrameSetStorage::view() const {
  validate(metadata);
  const auto image_pixels = pixel_count(metadata.left_rectified.grid);
  const auto matching_pixels = pixel_count(metadata.disparity.grid);
  const auto left_channels = channel_count(metadata.left_rectified.format);
  const auto right_channels = channel_count(metadata.right_rectified.format);
  if (left_rectified.size() != image_pixels * left_channels ||
      right_rectified.size() != image_pixels * right_channels ||
      disparity.size() != matching_pixels || depth_m.size() != matching_pixels ||
      confidence.size() != matching_pixels || validity.size() != matching_pixels) {
    throw std::invalid_argument(
        "owning frame-set storage does not match its product descriptors");
  }
  ProductSetView result;
  result.metadata = metadata;
  result.left_rectified = {left_rectified.data(),
                           metadata.left_rectified.grid,
                           static_cast<std::size_t>(
                               metadata.left_rectified.grid.width) *
                               left_channels,
                           metadata.left_rectified.format};
  result.right_rectified = {right_rectified.data(),
                            metadata.right_rectified.grid,
                            static_cast<std::size_t>(
                                metadata.right_rectified.grid.width) *
                                right_channels,
                            metadata.right_rectified.format};
  result.disparity = {disparity.data(), metadata.disparity.grid,
                      metadata.disparity.grid.width};
  result.depth_m = {depth_m.data(), metadata.depth.grid,
                    metadata.depth.grid.width};
  result.confidence = {confidence.data(), metadata.disparity.grid,
                       metadata.disparity.grid.width};
  result.validity = {validity.data(), metadata.disparity.grid,
                     metadata.disparity.grid.width};
  validate(result);
  return result;
}

std::size_t FrameSetStorage::payload_bytes() const noexcept {
  return left_rectified.size() + right_rectified.size() + confidence.size() +
         validity.size() + disparity.size() * sizeof(float) +
         depth_m.size() * sizeof(float);
}

NativeProcessingPipeline::NativeProcessingPipeline(
    NativePipelineConfiguration configuration,
    std::unique_ptr<RectificationBackend> rectifier,
    std::unique_ptr<NativeDisparityBackend> stereo)
    : configuration_(std::move(configuration)),
      rectifier_(std::move(rectifier)),
      stereo_(std::move(stereo)) {
  validate(configuration_);
  if (!rectifier_ || !stereo_) {
    throw std::invalid_argument(
        "native pipeline requires rectification and stereo backends");
  }
  const auto disparity = stereo_->metadata();
  validate(disparity);
  if (!same_grid(disparity.grid, configuration_.matching_calibration.grid)) {
    throw std::invalid_argument(
        "stereo native grid does not match the matching calibration");
  }
}

std::shared_ptr<const FrameSetStorage> NativeProcessingPipeline::process(
    StereoProcessingRequest request) {
  validate(request);
  if (request.left.width != configuration_.image_calibration.grid.width ||
      request.left.height != configuration_.image_calibration.grid.height) {
    throw std::invalid_argument(
        "capture geometry does not match the prepared image calibration");
  }

  const auto started = Clock::now();
  stereo_->refresh_configuration();
  auto output = std::make_shared<FrameSetStorage>();
  const auto& image_grid = configuration_.image_calibration.grid;
  const auto disparity_metadata = stereo_->metadata();
  const auto& matching_grid = disparity_metadata.grid;
  const auto image_pixels = pixel_count(image_grid);
  const auto matching_pixels = pixel_count(matching_grid);
  const auto channels = channel_count(request.left.encoding);
  output->left_rectified.resize(image_pixels * channels);
  output->right_rectified.resize(image_pixels * channels);
  output->disparity.resize(matching_pixels);
  output->depth_m.resize(matching_pixels);
  output->confidence.resize(matching_pixels);
  output->validity.resize(matching_pixels);
  std::vector<std::uint8_t> disparity_validity(matching_pixels);

  MutableImageView left_rectified{output->left_rectified.data(),
                                  image_grid.width, image_grid.height,
                                  image_grid.width * channels,
                                  request.left.encoding};
  MutableImageView right_rectified{output->right_rectified.data(),
                                   image_grid.width, image_grid.height,
                                   image_grid.width * channels,
                                   request.right.encoding};
  if (request.dmabuf && rectifier_->supports_dmabuf_pair()) {
    rectifier_->rectify_dmabuf_pair(*request.dmabuf, left_rectified,
                                    right_rectified,
                                    output->stats.rectification);
  } else {
    std::optional<CpuFallbackStereoInput> fallback;
    if (request.left.data == nullptr || request.right.data == nullptr) {
      if (!request.dmabuf || !request.dmabuf->acquire_cpu_fallback) {
        throw std::runtime_error("rectification backend requires CPU mappings");
      }
      fallback.emplace(request.dmabuf->acquire_cpu_fallback());
      request.left_mapping_owner = fallback->left_owner;
      request.right_mapping_owner = fallback->right_owner;
      request.left = fallback->left;
      request.right = fallback->right;
    }
    rectifier_->rectify_pair(request.left, request.right, left_rectified,
                             right_rectified, output->stats.rectification);
  }

  // No stage below this point reads camera memory. Requeue both capture
  // requests now so the next hardware trigger can run while SGBM/depth work
  // continues on the owned rectified images.
  release_rectified_capture_input(request);

  const ImageView left_for_stereo{left_rectified.data, left_rectified.width,
                                  left_rectified.height,
                                  left_rectified.stride_bytes,
                                  left_rectified.encoding};
  const ImageView right_for_stereo{right_rectified.data, right_rectified.width,
                                   right_rectified.height,
                                   right_rectified.stride_bytes,
                                   right_rectified.encoding};
  MutableDisparityProductsView disparity_output;
  disparity_output.disparity = {output->disparity.data(), matching_grid,
                                matching_grid.width};
  disparity_output.confidence = {output->confidence.data(), matching_grid,
                                 matching_grid.width};
  disparity_output.validity = {disparity_validity.data(), matching_grid,
                               matching_grid.width};
  stereo_->process_native(left_for_stereo, right_for_stereo, disparity_output,
                          output->stats.stereo);

  output->stats.depth = convert_disparity_to_depth(
      {output->disparity.data(), matching_grid, matching_grid.width},
      {disparity_validity.data(), matching_grid, matching_grid.width},
      configuration_.matching_calibration, configuration_.depth,
      {output->depth_m.data(), matching_grid, matching_grid.width},
      {output->validity.data(), matching_grid, matching_grid.width});

  output->metadata.frame = make_frame_set_identity(
      request.capture, configuration_.device_uuid, configuration_.run_uuid,
      configuration_.configuration_epoch);
  output->metadata.calibration = configuration_.image_calibration.identity;
  output->metadata.calibration_authority =
      configuration_.image_calibration.authority;
  const auto format = product_format(request.left.encoding);
  output->metadata.left_rectified = {
      image_grid, {0U, 0U, image_grid.width, image_grid.height},
      CameraRole::kLogicalLeft, format};
  output->metadata.right_rectified = {
      image_grid, {0U, 0U, image_grid.width, image_grid.height},
      CameraRole::kLogicalRight, format};
  output->metadata.disparity = disparity_metadata;
  output->metadata.depth = {matching_grid,
                            configuration_.depth.minimum_depth_m,
                            configuration_.depth.maximum_depth_m};
  const auto rectification_diagnostics = rectifier_->dmabuf_diagnostics();
  output->rectification_backend = rectification_diagnostics.selected_backend.empty()
                                      ? std::string(rectifier_->name())
                                      : rectification_diagnostics.selected_backend;
  output->rectification_fallback_reason =
      rectification_diagnostics.fallback_reason;
  output->stereo_backend = std::string(stereo_->name());
  output->stats.total_ms = elapsed_ms(started);
  validate(output->metadata, configuration_.image_calibration,
           configuration_.matching_calibration);
  (void)output->view();
  return output;
}

BoundedNativeScheduler::BoundedNativeScheduler(
    const SchedulerConfiguration configuration,
    std::unique_ptr<NativeProcessingPipeline> pipeline)
    : configuration_(configuration), pipeline_(std::move(pipeline)) {
  if (!pipeline_) {
    throw std::invalid_argument("native scheduler requires a pipeline");
  }
  if (configuration_.pending_capacity == 0U ||
      configuration_.completed_capacity == 0U ||
      configuration_.pending_capacity > 64U ||
      configuration_.completed_capacity > 64U) {
    throw std::invalid_argument("native scheduler queue capacities must be 1..64");
  }
  stats_.running = true;
  worker_ = std::thread(&BoundedNativeScheduler::worker_loop, this);
}

BoundedNativeScheduler::~BoundedNativeScheduler() noexcept { stop(); }

SubmitResult BoundedNativeScheduler::try_submit(
    StereoProcessingRequest request) {
  validate(request);
  std::optional<StereoProcessingRequest> replaced;
  {
    std::lock_guard lock(mutex_);
    if (stop_requested_) {
      return SubmitResult::kStopped;
    }
    if (pending_.size() >= configuration_.pending_capacity) {
      replaced.emplace(std::move(pending_.front()));
      pending_.pop_front();
      ++stats_.pending_replaced;
    }
    pending_.push_back(std::move(request));
    ++stats_.accepted;
    stats_.pending = pending_.size();
  }
  input_ready_.notify_one();
  return SubmitResult::kAccepted;
}

std::shared_ptr<const FrameSetStorage> BoundedNativeScheduler::wait_next(
    const std::chrono::milliseconds timeout) {
  if (timeout.count() < 0) {
    throw std::invalid_argument("scheduler wait timeout cannot be negative");
  }
  std::unique_lock lock(mutex_);
  output_ready_.wait_for(lock, timeout, [this] {
    return !completed_.empty() || !last_error_.empty() || stop_requested_;
  });
  if (completed_.empty()) {
    return {};
  }
  auto result = std::move(completed_.front());
  completed_.pop_front();
  stats_.completed = completed_.size();
  return result;
}

SchedulerStats BoundedNativeScheduler::stats() const noexcept {
  std::lock_guard lock(mutex_);
  auto result = stats_;
  result.pending = pending_.size();
  result.completed = completed_.size();
  result.running = !stop_requested_;
  return result;
}

std::string BoundedNativeScheduler::take_last_error() {
  std::lock_guard lock(mutex_);
  return std::exchange(last_error_, {});
}

void BoundedNativeScheduler::stop() noexcept {
  std::deque<StereoProcessingRequest> abandoned;
  {
    std::lock_guard lock(mutex_);
    if (stop_requested_) {
      return;
    }
    stop_requested_ = true;
    stats_.running = false;
    abandoned.swap(pending_);
  }
  input_ready_.notify_all();
  output_ready_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void BoundedNativeScheduler::worker_loop() noexcept {
  while (true) {
    std::optional<StereoProcessingRequest> request;
    {
      std::unique_lock lock(mutex_);
      input_ready_.wait(lock,
                        [this] { return stop_requested_ || !pending_.empty(); });
      if (stop_requested_) {
        return;
      }
      request.emplace(std::move(pending_.front()));
      pending_.pop_front();
      stats_.pending = pending_.size();
    }

    try {
      auto result = pipeline_->process(std::move(*request));
      std::lock_guard lock(mutex_);
      if (stop_requested_) {
        return;
      }
      if (completed_.size() >= configuration_.completed_capacity) {
        completed_.pop_front();
        ++stats_.completed_dropped;
      }
      completed_.push_back(std::move(result));
      ++stats_.processed;
      stats_.completed = completed_.size();
      output_ready_.notify_one();
    } catch (const std::exception& error) {
      std::lock_guard lock(mutex_);
      ++stats_.processing_failures;
      last_error_ = error.what();
      output_ready_.notify_all();
    } catch (...) {
      std::lock_guard lock(mutex_);
      ++stats_.processing_failures;
      last_error_ = "unknown native processing failure";
      output_ready_.notify_all();
    }
  }
}

}  // namespace depthd
