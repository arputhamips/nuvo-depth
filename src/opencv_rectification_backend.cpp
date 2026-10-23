#include "depthd/opencv_rectification_backend.hpp"
#include "depthd/opencv_execution.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace depthd {
namespace {

using Clock = std::chrono::steady_clock;

std::size_t channel_count(ImageEncoding encoding) {
  switch (encoding) {
    case ImageEncoding::kGray8:
      return 1;
    case ImageEncoding::kBgr8:
    case ImageEncoding::kRgb8:
      return 3;
  }
  throw std::invalid_argument("unsupported rectification image encoding");
}

int cv_type(ImageEncoding encoding) {
  return encoding == ImageEncoding::kGray8 ? CV_8UC1 : CV_8UC3;
}

double milliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

void validate_config(const RectificationBackendConfig& config,
                     const StereoRectificationMaps& maps) {
  if (config.input_width == 0 || config.input_height == 0 ||
      config.output_width == 0 || config.output_height == 0) {
    throw std::invalid_argument("rectification dimensions must be nonzero");
  }
  if (config.interpolation != RectificationInterpolation::kLinear) {
    throw std::invalid_argument("unsupported rectification interpolation");
  }
  if (!is_valid(maps) || maps.left.width != config.output_width ||
      maps.left.height != config.output_height) {
    throw std::invalid_argument(
        "rectification maps do not match configured output geometry");
  }
}

void validate_views(const RectificationBackendConfig& config,
                    const ImageView& input,
                    const MutableImageView& output) {
  if (input.data == nullptr || output.data == nullptr) {
    throw std::invalid_argument("rectification image data cannot be null");
  }
  if (input.width != config.input_width ||
      input.height != config.input_height ||
      output.width != config.output_width ||
      output.height != config.output_height) {
    throw std::invalid_argument("rectification image geometry mismatch");
  }
  if (input.encoding != output.encoding) {
    throw std::invalid_argument("rectification cannot change image encoding");
  }
  const auto channels = channel_count(input.encoding);
  const auto address_range = [channels](const void* data, const std::size_t width,
                                       const std::size_t height, const std::size_t stride) {
    const auto maximum_size = std::numeric_limits<std::size_t>::max();
    if (width > maximum_size / channels) {
      throw std::invalid_argument("rectification row size overflows");
    }
    const auto row_bytes = width * channels;
    if (stride < row_bytes) {
      throw std::invalid_argument("rectification image stride is too small");
    }
    if (height - 1U > (maximum_size - row_bytes) / stride) {
      throw std::invalid_argument("rectification image span overflows");
    }
    const auto extent = (height - 1U) * stride + row_bytes;
    const auto begin = reinterpret_cast<std::uintptr_t>(data);
    if (extent > std::numeric_limits<std::uintptr_t>::max() - begin) {
      throw std::invalid_argument("rectification address span overflows");
    }
    return std::pair{begin, begin + extent};
  };
  const auto source = address_range(input.data, input.width, input.height, input.stride_bytes);
  const auto destination = address_range(output.data, output.width, output.height, output.stride_bytes);
  if (source.first < destination.second && destination.first < source.second) {
    throw std::invalid_argument("rectification input and output must not overlap");
  }
}

struct FixedPointMap {
  cv::Mat coordinates;
  cv::Mat interpolation;
};

FixedPointMap make_fixed_point_map(const RectificationMap& map) {
  cv::Mat source_x(static_cast<int>(map.height), static_cast<int>(map.width),
                   CV_32FC1, const_cast<float*>(map.source_x.data()));
  cv::Mat source_y(static_cast<int>(map.height), static_cast<int>(map.width),
                   CV_32FC1, const_cast<float*>(map.source_y.data()));
  FixedPointMap fixed;
  cv::convertMaps(source_x, source_y, fixed.coordinates, fixed.interpolation,
                  CV_16SC2);
  return fixed;
}

}  // namespace

struct PreparedOpenCvRectificationMaps {
  std::uint32_t width{};
  std::uint32_t height{};
  FixedPointMap left;
  FixedPointMap right;
};

std::shared_ptr<const PreparedOpenCvRectificationMaps>
prepare_opencv_rectification_maps(const StereoRectificationMaps& maps) {
  if (!is_valid(maps)) throw std::invalid_argument("invalid CPU rectification maps");
  const auto execution = opencv_execution::compute_guard();
  auto prepared = std::make_shared<PreparedOpenCvRectificationMaps>();
  prepared->width = maps.left.width;
  prepared->height = maps.left.height;
  prepared->left = make_fixed_point_map(maps.left);
  prepared->right = make_fixed_point_map(maps.right);
  return prepared;
}

namespace {
class OpenCvRectificationBackend final : public RectificationBackend {
 public:
  OpenCvRectificationBackend(RectificationBackendConfig config,
                             std::shared_ptr<const PreparedOpenCvRectificationMaps> maps)
      : config_(config), maps_(std::move(maps)) {
    if (!maps_ || config_.input_width == 0U || config_.input_height == 0U ||
        config_.output_width != maps_->width || config_.output_height != maps_->height ||
        config_.interpolation != RectificationInterpolation::kLinear) {
      throw std::invalid_argument("prepared CPU maps do not match rectification geometry");
    }
  }

  [[nodiscard]] std::string_view name() const noexcept override {
    return kOpenCvRectificationBackendName;
  }

  void rectify(RectificationCamera camera, const ImageView& input,
               const MutableImageView& output,
               RectificationStats& stats) override {
    const auto execution = opencv_execution::compute_guard();
    validate_views(config_, input, output);
    const auto started = Clock::now();
    const auto type = cv_type(input.encoding);
    cv::Mat source(static_cast<int>(input.height), static_cast<int>(input.width),
                   type, const_cast<std::uint8_t*>(input.data),
                   input.stride_bytes);
    cv::Mat destination(static_cast<int>(output.height),
                        static_cast<int>(output.width), type, output.data,
                        output.stride_bytes);
    const auto& map = camera == RectificationCamera::kLeft ? maps_->left : maps_->right;
    stats = {};
    if (input.memory == ImageMemory::kDmaMapping) {
      // Preserve the measured capture staging policy. Owned RGB/preview images
      // already reside in ordinary CPU memory and need no additional copy.
      auto& staging = camera == RectificationCamera::kLeft ? left_staging_ : right_staging_;
      const auto transfer_started = Clock::now();
      source.copyTo(staging);
      stats.input_bytes_copied = static_cast<std::uint64_t>(input.width) *
          input.height * channel_count(input.encoding);
      stats.input_transfer_ms = milliseconds(transfer_started);
      source = staging;
    }
    const auto compute_started = Clock::now();
    cv::remap(source, destination, map.coordinates, map.interpolation,
              cv::INTER_LINEAR, cv::BORDER_CONSTANT,
              cv::Scalar::all(config_.border_value));
    stats.compute_ms = milliseconds(compute_started);
    stats.total_ms = milliseconds(started);
  }

 private:
  RectificationBackendConfig config_;
  std::shared_ptr<const PreparedOpenCvRectificationMaps> maps_;
  cv::Mat left_staging_;
  cv::Mat right_staging_;
};

}  // namespace

std::unique_ptr<RectificationBackend> make_opencv_rectification_backend(
    const RectificationBackendConfig& config,
    std::shared_ptr<const PreparedOpenCvRectificationMaps> maps) {
  return std::make_unique<OpenCvRectificationBackend>(config, std::move(maps));
}

void register_opencv_rectification_backend(
    RectificationBackendRegistry& registry) {
  registry.register_backend(
      std::string(kOpenCvRectificationBackendName),
      [](const RectificationBackendConfig& config,
         const StereoRectificationMaps& maps) {
        validate_config(config, maps);
        return make_opencv_rectification_backend(config, prepare_opencv_rectification_maps(maps));
      });
}

}  // namespace depthd
