#include "depthd/opencv_rectification_backend.hpp"
#include "depthd/opencv_execution.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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
  if (input.data == output.data) {
    throw std::invalid_argument("in-place rectification is not supported");
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
  if (input.stride_bytes < static_cast<std::size_t>(input.width) * channels ||
      output.stride_bytes <
          static_cast<std::size_t>(output.width) * channels) {
    throw std::invalid_argument("rectification image stride is too small");
  }
}

struct FixedPointMap {
  cv::Mat coordinates;
  cv::Mat interpolation;
  cv::Mat cached_input;
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

class OpenCvRectificationBackend final : public RectificationBackend {
 public:
  OpenCvRectificationBackend(RectificationBackendConfig config,
                             const StereoRectificationMaps& maps)
      : config_(config) {
    const auto execution = opencv_execution::compute_guard();
    validate_config(config_, maps);
    left_map_ = make_fixed_point_map(maps.left);
    right_map_ = make_fixed_point_map(maps.right);
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
    auto& map = camera == RectificationCamera::kLeft ? left_map_
                                                           : right_map_;
    // Camera DMA mappings are uncached on Pi5. One sequential read into reused
    // normal RAM avoids the much slower random reads performed by remap.
    // copyTo reallocates only when geometry or channel count changes.
    source.copyTo(map.cached_input);
    cv::remap(map.cached_input, destination, map.coordinates, map.interpolation,
              cv::INTER_LINEAR, cv::BORDER_CONSTANT,
              cv::Scalar::all(config_.border_value));
    stats = {};
    stats.compute_ms = milliseconds(started);
    stats.total_ms = stats.compute_ms;
  }

 private:
  RectificationBackendConfig config_;
  FixedPointMap left_map_;
  FixedPointMap right_map_;
};

}  // namespace

void register_opencv_rectification_backend(
    RectificationBackendRegistry& registry) {
  registry.register_backend(
      std::string(kOpenCvRectificationBackendName),
      [](const RectificationBackendConfig& config,
         const StereoRectificationMaps& maps) {
        return std::make_unique<OpenCvRectificationBackend>(config, maps);
      });
}

}  // namespace depthd
