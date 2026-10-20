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
  if (request.products == 0U || (request.products & ~ipc_v2::kSupportedProducts) != 0U) {
    throw std::invalid_argument("processing output selection is invalid");
  }
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
  if (request.color_preview == ColorPreviewRequest::kNone) {
    if (!request.color_preview_yuv420.empty()) {
      throw std::invalid_argument(
          "unrequested color preview payload must be empty");
    }
  } else {
    const auto color_pixels = static_cast<std::uint64_t>(request.left.width) *
                              request.left.height;
    if (request.left.width % 2U != 0U || request.left.height % 2U != 0U ||
        color_pixels > std::numeric_limits<std::size_t>::max() / 3U * 2U ||
        (!request.color_preview_yuv420.empty() &&
         request.color_preview_yuv420.size() != color_pixels * 3U / 2U)) {
      throw std::invalid_argument(
          "requested color preview payload is not packed YUV420");
    }
  }
}

void validate(const NativePipelineConfiguration& configuration) {
  if (configuration.frame_pool_capacity == 0U ||
      configuration.frame_pool_capacity > 64U) {
    throw std::invalid_argument("native frame storage capacity must be 1..64");
  }
  if (!uuid_present(configuration.device_uuid) ||
      !uuid_present(configuration.run_uuid) ||
      configuration.configuration_epoch == 0U) {
    throw std::invalid_argument(
        "native pipeline requires device, run, and configuration identity");
  }
  validate(configuration.image_calibration);
  validate(configuration.matching_calibration);
  constexpr std::size_t maximum_payload_budget = 256U * 1024U * 1024U;
  const auto budget = configuration.frame_pool_payload_budget_bytes;
  const auto image_pixels = pixel_count(configuration.image_calibration.grid);
  const auto matching_pixels =
      pixel_count(configuration.matching_calibration.grid);
  if (budget == 0U || budget > maximum_payload_budget ||
      matching_pixels > budget / 11U) {
    throw std::invalid_argument("native frame storage payload budget is invalid");
  }
  // All fourteen selectable outputs plus optional I420 previews occupy at
  // most 31 image bytes/pixel and 10 matching bytes/pixel per slot. Shared
  // scratch retains dependency planes for unrequested matcher/depth outputs
  // (11 matching bytes/pixel including native disparity validity).
  // Subtract shared scratch first and use division to
  // reject oversized grids without overflowing any intermediate product.
  const auto slot_budget =
      (budget - matching_pixels * 11U) / configuration.frame_pool_capacity;
  if (image_pixels > slot_budget / 31U ||
      matching_pixels > (slot_budget - image_pixels * 31U) / 10U) {
    throw std::invalid_argument(
        "native frame pool geometry exceeds its payload budget");
  }
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
  using namespace ipc_v2;
  if (available_products == 0U || (available_products & ~kSupportedProducts) != 0U)
    throw std::invalid_argument("owning frame-set product mask is invalid");
  const auto check = [&](const ProductKind kind, const std::size_t actual,
                         const std::size_t expected) {
    if (actual != ((available_products & product_bit(kind)) != 0U ? expected : 0U))
      throw std::invalid_argument("owning frame-set storage does not match its product descriptors");
  };
  check(ProductKind::kLeftRectified, left_rectified.size(), image_pixels * left_channels);
  check(ProductKind::kRightRectified, right_rectified.size(), image_pixels * right_channels);
  check(ProductKind::kDisparity, disparity.size(), matching_pixels);
  check(ProductKind::kDepth, depth_m.size(), matching_pixels);
  check(ProductKind::kConfidence, confidence.size(), matching_pixels);
  check(ProductKind::kValidity, validity.size(), matching_pixels);
  check(ProductKind::kLeftMono, left_mono.size(), image_pixels);
  check(ProductKind::kRightMono, right_mono.size(), image_pixels);
  check(ProductKind::kLeftRgb, left_rgb.size(), image_pixels * 3U);
  check(ProductKind::kRightRgb, right_rgb.size(), image_pixels * 3U);
  check(ProductKind::kLeftRectifiedRgb, left_rectified_rgb.size(), image_pixels * 3U);
  check(ProductKind::kRightRectifiedRgb, right_rectified_rgb.size(), image_pixels * 3U);
  check(ProductKind::kRectifiedStereo, rectified_stereo.size(), image_pixels * 2U);
  check(ProductKind::kRectifiedStereoRgb, rectified_stereo_rgb.size(), image_pixels * 6U);
  ProductSetView result;
  result.metadata = metadata;
  result.available_products = available_products;
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
         validity.size() + left_color_yuv420.size() +
         right_color_yuv420.size() + disparity.size() * sizeof(float) +
         depth_m.size() * sizeof(float) + left_mono.size() + right_mono.size() +
         left_rgb.size() + right_rgb.size() + left_rectified_rgb.size() +
         right_rectified_rgb.size() + rectified_stereo.size() + rectified_stereo_rgb.size();
}

NativeProcessingPipeline::NativeProcessingPipeline(
    NativePipelineConfiguration configuration,
    std::unique_ptr<RectificationBackend> rectifier,
    std::unique_ptr<NativeDisparityBackend> stereo,
    std::unique_ptr<RectificationBackend> rgb_rectifier)
    : configuration_(std::move(configuration)),
      rectifier_(std::move(rectifier)),
      rgb_rectifier_(std::move(rgb_rectifier)),
      stereo_(std::move(stereo)) {
  validate(configuration_);
  if (!rectifier_ || !stereo_) {
    throw std::invalid_argument(
        "native pipeline requires rectification and stereo backends");
  }
  stereo_generation_ = stereo_->configuration_state().generation;
  const auto disparity = stereo_->metadata();
  validate(disparity);
  if (!same_grid(disparity.grid, configuration_.matching_calibration.grid)) {
    throw std::invalid_argument(
        "stereo native grid does not match the matching calibration");
  }
  frame_pool_.reserve(configuration_.frame_pool_capacity);
  for (std::size_t slot = 0; slot < configuration_.frame_pool_capacity; ++slot) {
    frame_pool_.push_back(std::make_shared<FrameSetStorage>());
  }
}

bool NativeProcessingPipeline::has_available_storage() const noexcept {
  return std::any_of(frame_pool_.begin(), frame_pool_.end(),
                     [](const auto& slot) { return slot.use_count() == 1; });
}

std::shared_ptr<const FrameSetStorage> NativeProcessingPipeline::process(
    StereoProcessingRequest request) {
  validate(request);
  if (request.left.width != configuration_.image_calibration.grid.width ||
      request.left.height != configuration_.image_calibration.grid.height) {
    throw std::invalid_argument(
        "capture geometry does not match the prepared image calibration");
  }

  // The pool keeps one owner of each slot. Only a sole-owned slot can be
  // changed; shared_ptr consumers may release on other threads and keep their
  // complete frame alive even after this pipeline is destroyed.
  const auto available =
      std::find_if(frame_pool_.begin(), frame_pool_.end(),
                   [](const auto& slot) { return slot.use_count() == 1; });
  if (available == frame_pool_.end()) {
    release_rectified_capture_input(request);
    return {};
  }
  auto output = *available;
  output->metadata = {};
  output->available_products = request.products;
  output->publication_context = std::move(request.publication_context);
  output->stats = {};
  output->left_color_yuv420.clear();
  output->right_color_yuv420.clear();

  const auto started = Clock::now();
  stereo_->refresh_configuration();
  output->stereo_configuration = stereo_->configuration_state();
  if (configuration_.configuration_epochs) {
    const auto epoch = configuration_.configuration_epochs->observe(
        output->stereo_configuration.effective_revision.empty()
            ? output->stereo_configuration.applied_revision
            : output->stereo_configuration.effective_revision,
        request.capture.capture_timestamp_ns);
    if (!epoch) return {};
    configuration_.configuration_epoch = *epoch;
  } else if (output->stereo_configuration.generation != stereo_generation_) {
    if (configuration_.configuration_epoch ==
        std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("configuration epoch exhausted");
    }
    ++configuration_.configuration_epoch;
    stereo_generation_ = output->stereo_configuration.generation;
  }
  const auto& image_grid = configuration_.image_calibration.grid;
  const auto disparity_metadata = stereo_->metadata();
  validate(disparity_metadata);
  if (!same_grid(disparity_metadata.grid,
                 configuration_.matching_calibration.grid)) {
    throw std::invalid_argument(
        "refreshed stereo grid does not match the prepared calibration");
  }
  const auto& matching_grid = disparity_metadata.grid;
  const auto image_pixels = pixel_count(image_grid);
  const auto matching_pixels = pixel_count(matching_grid);
  // Camera views are cleared when their leases are released below. Keep the
  // format describing the owned rectified bytes independently of those views.
  const auto image_format = product_format(request.left.encoding);
  const auto channels = channel_count(request.left.encoding);
  using namespace ipc_v2;
  const auto has = [&](const ProductKind kind) {
    return (request.products & product_bit(kind)) != 0U;
  };
  const bool match = (request.products & kMatcherProducts) != 0U;
  const bool metric = has(ProductKind::kDepth) || has(ProductKind::kValidity);
  const bool rectify_mono = match || has(ProductKind::kLeftRectified) ||
      has(ProductKind::kRightRectified) || has(ProductKind::kRectifiedStereo);
  const bool rectify_rgb = has(ProductKind::kLeftRectifiedRgb) ||
      has(ProductKind::kRightRectifiedRgb) || has(ProductKind::kRectifiedStereoRgb);
  const bool left_rect = match || has(ProductKind::kLeftRectified) || has(ProductKind::kRectifiedStereo);
  const bool right_rect = match || has(ProductKind::kRightRectified) || has(ProductKind::kRectifiedStereo);
  const bool left_rgb_rect = has(ProductKind::kLeftRectifiedRgb) || has(ProductKind::kRectifiedStereoRgb);
  const bool right_rgb_rect = has(ProductKind::kRightRectifiedRgb) || has(ProductKind::kRectifiedStereoRgb);
  auto resize = [](auto& buffer, const std::size_t count, const std::size_t capacity) {
    buffer.reserve(capacity);
    buffer.resize(count);
  };
  const auto acquire_cpu = [&] {
    if (request.left.data == nullptr || request.right.data == nullptr) {
      if (!request.dmabuf || !request.dmabuf->acquire_cpu_fallback) {
        throw std::runtime_error("rectification backend requires CPU mappings");
      }
      const auto fallback = request.dmabuf->acquire_cpu_fallback();
      request.left_mapping_owner = fallback.left_owner;
      request.right_mapping_owner = fallback.right_owner;
      request.left = fallback.left;
      request.right = fallback.right;
    }
  };
  for (const auto role : {CameraRole::kLogicalLeft, CameraRole::kLogicalRight}) {
    const bool left = role == CameraRole::kLogicalLeft;
    if (has(left ? ProductKind::kLeftMono : ProductKind::kRightMono)) {
      auto& buffer = left ? output->left_mono : output->right_mono;
      resize(buffer, image_pixels, image_pixels);
      copy_capture_image(request, role, false, buffer);
    }
    if ((left ? left_rgb_rect : right_rgb_rect) || has(left ? ProductKind::kLeftRgb : ProductKind::kRightRgb)) {
      auto& buffer = left ? output->left_rgb : output->right_rgb;
      resize(buffer, image_pixels * 3U, image_pixels * 3U);
      copy_capture_rgb(request, role, buffer);
    }
  }
  MutableImageView left_rectified{}, right_rectified{};
  if (rectify_mono) {
    if (left_rect) resize(output->left_rectified, image_pixels * channels, image_pixels * 3U);
    if (right_rect) resize(output->right_rectified, image_pixels * channels, image_pixels * 3U);
    left_rectified = {output->left_rectified.data(), image_grid.width,
                      image_grid.height, image_grid.width * channels, request.left.encoding};
    right_rectified = {output->right_rectified.data(), image_grid.width,
                       image_grid.height, image_grid.width * channels, request.right.encoding};
    if (left_rect && right_rect && request.dmabuf && rectifier_->supports_dmabuf_pair() && channels == 1U) {
      rectifier_->rectify_dmabuf_pair(*request.dmabuf, left_rectified, right_rectified,
                                    output->stats.rectification);
    } else {
      acquire_cpu();
      if (left_rect && right_rect) {
        rectifier_->rectify_pair(request.left, request.right, left_rectified,
                                right_rectified, output->stats.rectification);
      } else {
        auto& backend = rgb_rectifier_ ? *rgb_rectifier_ : *rectifier_;
        backend.rectify(left_rect ? RectificationCamera::kLeft : RectificationCamera::kRight,
                        left_rect ? request.left : request.right,
                        left_rect ? left_rectified : right_rectified, output->stats.rectification);
      }
    }
  }
  if (rectify_rgb) {
    if (left_rgb_rect) resize(output->left_rectified_rgb, image_pixels * 3U, image_pixels * 3U);
    if (right_rgb_rect) resize(output->right_rectified_rgb, image_pixels * 3U, image_pixels * 3U);
    auto& backend = rgb_rectifier_ ? *rgb_rectifier_ : *rectifier_;
    RectificationStats rgb_stats{};
    const ImageView left_input{output->left_rgb.data(), image_grid.width, image_grid.height,
                              image_grid.width * 3U, ImageEncoding::kRgb8};
    const ImageView right_input{output->right_rgb.data(), image_grid.width, image_grid.height,
                               image_grid.width * 3U, ImageEncoding::kRgb8};
    const MutableImageView left_output{output->left_rectified_rgb.data(), image_grid.width,
        image_grid.height, image_grid.width * 3U, ImageEncoding::kRgb8};
    const MutableImageView right_output{output->right_rectified_rgb.data(), image_grid.width,
        image_grid.height, image_grid.width * 3U, ImageEncoding::kRgb8};
    if (left_rgb_rect && right_rgb_rect)
      backend.rectify_pair(left_input, right_input, left_output, right_output, rgb_stats);
    else backend.rectify(left_rgb_rect ? RectificationCamera::kLeft : RectificationCamera::kRight,
                         left_rgb_rect ? left_input : right_input,
                         left_rgb_rect ? left_output : right_output, rgb_stats);
    output->stats.rectification.input_transfer_ms += rgb_stats.input_transfer_ms;
    output->stats.rectification.compute_ms += rgb_stats.compute_ms;
    output->stats.rectification.output_transfer_ms += rgb_stats.output_transfer_ms;
    output->stats.rectification.total_ms += rgb_stats.total_ms;
  }
  if (request.color_preview != ColorPreviewRequest::kNone) {
    auto& buffer = request.color_preview == ColorPreviewRequest::kLeft ?
        output->left_color_yuv420 : output->right_color_yuv420;
    resize(buffer, image_pixels * 3U / 2U, image_pixels * 3U / 2U);
    copy_capture_image(request, request.color_preview == ColorPreviewRequest::kLeft ?
        CameraRole::kLogicalLeft : CameraRole::kLogicalRight, true, buffer);
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
  if (match) {
    auto& disparity = has(ProductKind::kDisparity) ? output->disparity : disparity_scratch_;
    auto& confidence = has(ProductKind::kConfidence) ? output->confidence : confidence_scratch_;
    resize(disparity, matching_pixels, matching_pixels);
    resize(confidence, matching_pixels, matching_pixels);
    disparity_validity_.resize(matching_pixels);
    stereo_->process_native(left_for_stereo, right_for_stereo,
        {{disparity.data(), matching_grid, matching_grid.width},
         {confidence.data(), matching_grid, matching_grid.width},
         {disparity_validity_.data(), matching_grid, matching_grid.width}}, output->stats.stereo);
    if (metric) {
      auto& depth = has(ProductKind::kDepth) ? output->depth_m : depth_scratch_;
      auto& validity = has(ProductKind::kValidity) ? output->validity : depth_validity_scratch_;
      resize(depth, matching_pixels, matching_pixels);
      resize(validity, matching_pixels, matching_pixels);
      output->stats.depth = convert_disparity_to_depth(
          {disparity.data(), matching_grid, matching_grid.width},
          {disparity_validity_.data(), matching_grid, matching_grid.width},
          configuration_.matching_calibration, configuration_.depth,
          {depth.data(), matching_grid, matching_grid.width},
          {validity.data(), matching_grid, matching_grid.width});
    }
  }
  const auto combine = [&](const auto& left, const auto& right, auto& target,
                           const std::size_t source_channels, const bool mono) {
    const auto target_channels = mono ? 1U : 3U;
    resize(target, image_pixels * 2U * target_channels, image_pixels * 2U * target_channels);
    for (std::size_t row = 0; row < image_grid.height; ++row) {
      for (std::size_t side = 0; side < 2U; ++side) {
        const auto& source = side == 0U ? left : right;
        for (std::size_t column = 0; column < image_grid.width; ++column) {
          const auto input = (row * image_grid.width + column) * source_channels;
          const auto dest = ((row * 2U + side) * image_grid.width + column) * target_channels;
          if (mono && source_channels == 3U) {
            const auto red = image_format == ImageProductFormat::kRgb8 ? 0U : 2U;
            target[dest] = static_cast<std::uint8_t>((77U * source[input + red] +
                150U * source[input + 1U] + 29U * source[input + (2U - red)] + 128U) >> 8U);
          } else {
            std::copy_n(source.data() + input, target_channels, target.data() + dest);
          }
        }
      }
    }
  };
  if (has(ProductKind::kRectifiedStereo))
    combine(output->left_rectified, output->right_rectified, output->rectified_stereo, channels, true);
  if (has(ProductKind::kRectifiedStereoRgb))
    combine(output->left_rectified_rgb, output->right_rectified_rgb, output->rectified_stereo_rgb, 3U, false);
  // Dependency planes may occupy reusable capacity but are never published as
  // unwanted products, and stale selections cannot survive slot reuse.
  const auto clear_unselected = [&](const ProductKind kind, auto& buffer) {
    if (!has(kind)) buffer.clear();
  };
  clear_unselected(ProductKind::kLeftRectified, output->left_rectified);
  clear_unselected(ProductKind::kRightRectified, output->right_rectified);
  clear_unselected(ProductKind::kDisparity, output->disparity);
  clear_unselected(ProductKind::kDepth, output->depth_m);
  clear_unselected(ProductKind::kConfidence, output->confidence);
  clear_unselected(ProductKind::kValidity, output->validity);
  clear_unselected(ProductKind::kLeftMono, output->left_mono);
  clear_unselected(ProductKind::kRightMono, output->right_mono);
  clear_unselected(ProductKind::kLeftRgb, output->left_rgb);
  clear_unselected(ProductKind::kRightRgb, output->right_rgb);
  clear_unselected(ProductKind::kLeftRectifiedRgb, output->left_rectified_rgb);
  clear_unselected(ProductKind::kRightRectifiedRgb, output->right_rectified_rgb);
  clear_unselected(ProductKind::kRectifiedStereo, output->rectified_stereo);
  clear_unselected(ProductKind::kRectifiedStereoRgb, output->rectified_stereo_rgb);

  output->metadata.frame = make_frame_set_identity(
      request.capture, configuration_.device_uuid, configuration_.run_uuid,
      configuration_.configuration_epoch);
  output->metadata.calibration = configuration_.image_calibration.identity;
  output->metadata.calibration_authority =
      configuration_.image_calibration.authority;
  output->metadata.left_rectified = {
      image_grid, {0U, 0U, image_grid.width, image_grid.height},
      CameraRole::kLogicalLeft, image_format};
  output->metadata.right_rectified = {
      image_grid, {0U, 0U, image_grid.width, image_grid.height},
      CameraRole::kLogicalRight, image_format};
  output->metadata.disparity = disparity_metadata;
  output->metadata.depth = {matching_grid,
                            configuration_.depth.minimum_depth_m,
                            configuration_.depth.maximum_depth_m};
  const auto rectification_diagnostics = rectifier_->dmabuf_diagnostics();
  if (!rectify_mono && !rectify_rgb) {
    output->rectification_backend = "disabled";
  } else if (rectify_rgb && !rectify_mono && rgb_rectifier_) {
    output->rectification_backend.assign(rgb_rectifier_->name());
  } else if (rectification_diagnostics.selected_backend.empty()) {
    output->rectification_backend.assign(rectifier_->name());
  } else {
    output->rectification_backend = rectification_diagnostics.selected_backend;
  }
  output->rectification_fallback_reason = rectify_mono ?
      rectification_diagnostics.fallback_reason : std::string{};
  output->stereo_backend.assign(match ? stereo_->name() : "disabled");
  output->stats.total_ms = elapsed_ms(started);
  validate(output->metadata, configuration_.image_calibration,
           configuration_.matching_calibration);
  (void)output->view();
  return output;
}

NativeConfigurationEpochs::NativeConfigurationEpochs(const std::uint64_t initial_epoch)
    : epoch_(initial_epoch) {
  if (epoch_ == 0U) throw std::invalid_argument("configuration epoch must be nonzero");
}

std::optional<std::uint64_t> NativeConfigurationEpochs::observe(
    const std::string_view revision, const std::int64_t capture_time_ns) {
  std::lock_guard lock(mutex_);
  for (auto item = revisions_.rbegin(); item != revisions_.rend(); ++item) {
    if (capture_time_ns < item->first_capture_time_ns) continue;
    if (item->value == revision) {
      newest_capture_time_ns_ = std::max(newest_capture_time_ns_, capture_time_ns);
      return item->epoch;
    }
    // A capture in an older interval cannot establish a new current revision.
    if (item != revisions_.rbegin()) return {};
    break;
  }
  if (!revisions_.empty()) {
    if (capture_time_ns < newest_capture_time_ns_) return {};
    if (epoch_ == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("configuration epoch exhausted");
    ++epoch_;
  }
  newest_capture_time_ns_ = capture_time_ns;
  if (revisions_.size() == 16U) revisions_.pop_front();
  revisions_.push_back({std::string(revision), epoch_, capture_time_ns});
  return epoch_;
}

BoundedNativeScheduler::BoundedNativeScheduler(
    const SchedulerConfiguration configuration,
    std::unique_ptr<NativeProcessingPipeline> pipeline,
    std::unique_ptr<NativeProcessingPipeline> image_pipeline)
    : configuration_(configuration), pipeline_(std::move(pipeline)),
      image_pipeline_(std::move(image_pipeline)) {
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
  worker_ = std::thread(&BoundedNativeScheduler::worker_loop, this, false);
  try {
    if (image_pipeline_)
      image_worker_ = std::thread(&BoundedNativeScheduler::worker_loop, this, true);
  } catch (...) {
    stop();
    throw;
  }
}

BoundedNativeScheduler::~BoundedNativeScheduler() noexcept { stop(); }

SubmitResult BoundedNativeScheduler::try_submit(
    StereoProcessingRequest request) {
  validate(request);
  const auto images = request.independent_image_products;
  if ((images & ipc_v2::kMatcherProducts) != 0U || (images & ~request.products) != 0U)
    throw std::invalid_argument("independent images must be an image subset of the request");
  const bool matcher = (request.products & ipc_v2::kMatcherProducts) != 0U;
  if (image_pipeline_ && matcher && images != 0U) {
    StereoProcessingRequest image;
    image.products = images;
    const auto share_frame = [](CapturedFrame& source) {
      CapturedFrame result;
      result.role = source.role;
      result.camera_stable_id = source.camera_stable_id;
      result.capture_epoch = source.capture_epoch;
      result.stream_sequence = source.stream_sequence;
      result.sensor_timestamp_ns = source.sensor_timestamp_ns;
      result.isp_exposure_time_us = source.isp_exposure_time_us;
      result.isp_analogue_gain = source.isp_analogue_gain;
      result.isp_digital_gain = source.isp_digital_gain;
      result.sensor_clock = source.sensor_clock;
      result.grid = source.grid;
      result.pixel_format = source.pixel_format;
      result.planes = source.planes;
      result.lease = source.lease.share();
      return result;
    };
    image.capture.left = share_frame(request.capture.left);
    image.capture.right = share_frame(request.capture.right);
    image.capture.capture_epoch = request.capture.capture_epoch;
    image.capture.pair_sequence = request.capture.pair_sequence;
    image.capture.capture_timestamp_ns = request.capture.capture_timestamp_ns;
    image.capture.sensor_delta_ns = request.capture.sensor_delta_ns;
    image.left_mapping_owner = request.left_mapping_owner;
    image.right_mapping_owner = request.right_mapping_owner;
    image.left = request.left;
    image.right = request.right;
    // Copy the lazy mapper so each worker owns its mutable mapping cache.
    image.dmabuf = request.dmabuf;
    image.left_dmabuf_lease = request.left_dmabuf_lease;
    image.right_dmabuf_lease = request.right_dmabuf_lease;
    image.publication_context = request.publication_context;
    (void)enqueue(std::move(image), true);
  }
  // Admission of a numeric preview refers to the matcher lane, even when its
  // independent image was admitted separately.
  return enqueue(std::move(request), image_pipeline_ && !matcher);
}

SubmitResult BoundedNativeScheduler::enqueue(
    StereoProcessingRequest request, const bool image_lane) {
  auto& pending = image_lane ? image_pending_ : pending_;
  std::optional<StereoProcessingRequest> replaced;
  {
    std::lock_guard lock(mutex_);
    if (stop_requested_) {
      return SubmitResult::kStopped;
    }
    if (pending.size() >= configuration_.pending_capacity) {
      auto victim = std::find_if(pending.begin(), pending.end(),
            [&request](const auto& queued) {
              return (!queued.preserve_for_preview || request.preserve_for_preview) &&
                  ((queued.products & ipc_v2::kMatcherProducts) == 0U ||
                   (request.products & queued.products) == queued.products);
            });
      if (victim == pending.end()) {
        ++stats_.protected_pending_dropped;
        return SubmitResult::kQueueFull;
      }
      replaced.emplace(std::move(*victim));
      pending.erase(victim);
      ++stats_.pending_replaced;
    }
    pending.push_back(std::move(request));
    ++stats_.accepted;

  }
  input_ready_.notify_all();
  return SubmitResult::kAccepted;
}

std::shared_ptr<const FrameSetStorage> BoundedNativeScheduler::wait_next(
    const std::chrono::milliseconds timeout) {
  if (timeout.count() < 0) {
    throw std::invalid_argument("scheduler wait timeout cannot be negative");
  }
  std::unique_lock lock(mutex_);
  output_ready_.wait_for(lock, timeout, [this] {
    return !completed_.empty() || !image_completed_.empty() || !last_error_.empty() || stop_requested_;
  });
  auto& completed = completed_.empty() ? image_completed_ : completed_;
  if (completed.empty()) return {};
  auto result = std::move(completed.front());
  completed.pop_front();

  return result;
}

SchedulerStats BoundedNativeScheduler::stats() const noexcept {
  std::lock_guard lock(mutex_);
  auto result = stats_;
  result.pending = pending_.size() + image_pending_.size();
  result.completed = completed_.size() + image_completed_.size();
  result.running = !stop_requested_;
  return result;
}

std::string BoundedNativeScheduler::take_last_error() {
  std::lock_guard lock(mutex_);
  return std::exchange(last_error_, {});
}

void BoundedNativeScheduler::stop() noexcept {
  std::deque<StereoProcessingRequest> abandoned;
  std::deque<StereoProcessingRequest> abandoned_images;
  {
    std::lock_guard lock(mutex_);
    if (stop_requested_) {
      return;
    }
    stop_requested_ = true;
    stats_.running = false;
    abandoned.swap(pending_);
    abandoned_images.swap(image_pending_);
  }
  input_ready_.notify_all();
  output_ready_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (image_worker_.joinable()) image_worker_.join();
}

void BoundedNativeScheduler::worker_loop(const bool image_lane) noexcept {
  auto& pending = image_lane ? image_pending_ : pending_;
  auto& completed = image_lane ? image_completed_ : completed_;
  auto& pipeline = image_lane ? image_pipeline_ : pipeline_;
  while (true) {
    std::optional<StereoProcessingRequest> request;
    {
      std::unique_lock lock(mutex_);
      input_ready_.wait(lock,
                        [this, &pending] { return stop_requested_ || !pending.empty(); });
      if (stop_requested_) {
        return;
      }
      request.emplace(std::move(pending.front()));
      pending.pop_front();

    }

    try {
      // Release stale queued completions before trying to reuse their storage.
      // Frames already handed to consumers remain immutable and pinned. If
      // those consume every slot, drop this input whole rather than allocate
      // more memory or wait while retaining camera requests.
      {
        std::lock_guard lock(mutex_);
        while (!pipeline->has_available_storage() && !completed.empty()) {
          completed.pop_front();
          ++stats_.completed_dropped;
        }

      }
      auto result = pipeline->process(std::move(*request));
      std::lock_guard lock(mutex_);
      if (stop_requested_) {
        return;
      }
      if (!result) {
        ++stats_.storage_exhausted_dropped;
        continue;
      }
      if (completed.size() >= configuration_.completed_capacity) {
        completed.pop_front();
        ++stats_.completed_dropped;
      }
      completed.push_back(std::move(result));
      ++stats_.processed;

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
