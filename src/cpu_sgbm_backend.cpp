#include "depthd/stereo_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace depthd {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double elapsed_ms(const Clock::time_point start,
                                const Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

[[nodiscard]] int cv_type(const ImageEncoding encoding) {
  switch (encoding) {
    case ImageEncoding::kGray8:
      return CV_8UC1;
    case ImageEncoding::kBgr8:
    case ImageEncoding::kRgb8:
      return CV_8UC3;
  }
  throw std::invalid_argument("unsupported image encoding");
}

void validate_config(const CpuSgbmConfig& config) {
  if (config.input_width == 0 || config.input_height == 0 ||
      config.working_width == 0 || config.working_height == 0) {
    throw std::invalid_argument("SGBM image dimensions must be non-zero");
  }
  if (config.working_width > config.input_width ||
      config.working_height > config.input_height) {
    throw std::invalid_argument("SGBM working dimensions cannot exceed input");
  }
  if (config.working_disparities <= 0 ||
      config.working_disparities % 16 != 0 ||
      config.working_disparities >= static_cast<std::int32_t>(config.working_width)) {
    throw std::invalid_argument(
        "SGBM working disparity count must be a positive multiple of 16 "
        "smaller than the working width");
  }
  if (config.full_resolution_disparities <= 0 || config.block_size < 1 ||
      config.block_size % 2 == 0) {
    throw std::invalid_argument("invalid SGBM disparity range or block size");
  }
  if (config.opencv_threads < 1) {
    throw std::invalid_argument("OpenCV thread count must be positive");
  }
}

class CpuSgbmBackend final : public StereoBackend {
 public:
  explicit CpuSgbmBackend(const CpuSgbmConfig& config) : config_(config) {
    validate_config(config_);
    cv::setNumThreads(config_.opencv_threads);

    const int channels = 1;
    const int window_area = config_.block_size * config_.block_size;
    matcher_ = cv::StereoSGBM::create(
        config_.min_disparity, config_.working_disparities, config_.block_size,
        8 * channels * window_area, 32 * channels * window_area,
        config_.disparity_12_max_diff, config_.pre_filter_cap,
        config_.uniqueness_ratio, config_.speckle_window_size,
        config_.speckle_range, cv::StereoSGBM::MODE_SGBM_3WAY);

    const cv::Size input_size(static_cast<int>(config_.input_width),
                              static_cast<int>(config_.input_height));
    const cv::Size working_size(static_cast<int>(config_.working_width),
                                static_cast<int>(config_.working_height));
    left_gray_.create(input_size, CV_8UC1);
    right_gray_.create(input_size, CV_8UC1);
    left_working_.create(working_size, CV_8UC1);
    right_working_.create(working_size, CV_8UC1);
    disparity_fixed_.create(working_size, CV_16SC1);
    disparity_working_.create(working_size, CV_32FC1);
    valid_working_.create(working_size, CV_8UC1);
    valid_full_.create(input_size, CV_8UC1);
    within_limit_full_.create(input_size, CV_8UC1);
  }

  [[nodiscard]] std::string_view name() const noexcept override {
    return "opencv_sgbm_cpu";
  }

  void process(const ImageView& left, const ImageView& right,
               const MutableDisparityView& output,
               StereoBackendStats& stats) override {
    validate_views(left, right, output);
    const auto started = Clock::now();

    to_gray(left, left_gray_);
    to_gray(right, right_gray_);
    cv::resize(left_gray_, left_working_, left_working_.size(), 0.0, 0.0,
               cv::INTER_AREA);
    cv::resize(right_gray_, right_working_, right_working_.size(), 0.0, 0.0,
               cv::INTER_AREA);
    const auto preprocessed = Clock::now();

    matcher_->compute(left_working_, right_working_, disparity_fixed_);
    const auto matched = Clock::now();

    const float minimum_useful_fixed =
        static_cast<float>(config_.min_disparity * 16);
    cv::compare(disparity_fixed_, minimum_useful_fixed, valid_working_, cv::CMP_GT);
    disparity_fixed_.convertTo(disparity_working_, CV_32F, 1.0 / 16.0);

    cv::Mat output_mat(static_cast<int>(output.height),
                       static_cast<int>(output.width), CV_32FC1, output.data,
                       output.stride_elements * sizeof(float));
    cv::resize(disparity_working_, output_mat, output_mat.size(), 0.0, 0.0,
               cv::INTER_LINEAR);
    const float disparity_scale =
        static_cast<float>(config_.input_width) /
        static_cast<float>(config_.working_width);
    output_mat *= disparity_scale;
    cv::resize(valid_working_, valid_full_, valid_full_.size(), 0.0, 0.0,
               cv::INTER_NEAREST);
    cv::compare(output_mat,
                static_cast<float>(config_.full_resolution_disparities),
                within_limit_full_, cv::CMP_LT);
    cv::bitwise_and(valid_full_, within_limit_full_, valid_full_);
    output_mat.setTo(std::numeric_limits<float>::quiet_NaN(),
                     valid_full_ == 0);

    const auto finished = Clock::now();
    stats.preprocess_ms = elapsed_ms(started, preprocessed);
    stats.match_ms = elapsed_ms(preprocessed, matched);
    stats.postprocess_ms = elapsed_ms(matched, finished);
    stats.total_ms = elapsed_ms(started, finished);
    stats.valid_fraction = static_cast<float>(cv::countNonZero(valid_full_)) /
                           static_cast<float>(valid_full_.total());
    const int searchable_start = std::clamp(
        static_cast<int>(std::ceil(
            static_cast<float>(
                config_.min_disparity + config_.working_disparities) *
            disparity_scale)),
        0, valid_full_.cols - 1);
    const cv::Mat searchable = valid_full_(
        cv::Rect(searchable_start, 0, valid_full_.cols - searchable_start,
                 valid_full_.rows));
    stats.searchable_valid_fraction =
        static_cast<float>(cv::countNonZero(searchable)) /
        static_cast<float>(searchable.total());
  }

 private:
  void validate_views(const ImageView& left, const ImageView& right,
                      const MutableDisparityView& output) const {
    const auto validate_input = [&](const ImageView& view,
                                    const std::string& side) {
      if (view.data == nullptr || view.width != config_.input_width ||
          view.height != config_.input_height) {
        throw std::invalid_argument(side + " image geometry is invalid");
      }
      const std::size_t channels =
          view.encoding == ImageEncoding::kGray8 ? 1U : 3U;
      if (view.stride_bytes < static_cast<std::size_t>(view.width) * channels) {
        throw std::invalid_argument(side + " image stride is invalid");
      }
    };
    validate_input(left, "left");
    validate_input(right, "right");
    if (output.data == nullptr || output.width != config_.input_width ||
        output.height != config_.input_height ||
        output.stride_elements < output.width) {
      throw std::invalid_argument("disparity output geometry is invalid");
    }
  }

  static void to_gray(const ImageView& view, cv::Mat& destination) {
    const cv::Mat source(static_cast<int>(view.height),
                         static_cast<int>(view.width), cv_type(view.encoding),
                         const_cast<std::uint8_t*>(view.data), view.stride_bytes);
    switch (view.encoding) {
      case ImageEncoding::kGray8:
        source.copyTo(destination);
        break;
      case ImageEncoding::kBgr8:
        cv::cvtColor(source, destination, cv::COLOR_BGR2GRAY);
        break;
      case ImageEncoding::kRgb8:
        cv::cvtColor(source, destination, cv::COLOR_RGB2GRAY);
        break;
    }
  }

  CpuSgbmConfig config_;
  cv::Ptr<cv::StereoSGBM> matcher_;
  cv::Mat left_gray_;
  cv::Mat right_gray_;
  cv::Mat left_working_;
  cv::Mat right_working_;
  cv::Mat disparity_fixed_;
  cv::Mat disparity_working_;
  cv::Mat valid_working_;
  cv::Mat valid_full_;
  cv::Mat within_limit_full_;
};

}  // namespace

std::unique_ptr<StereoBackend> make_cpu_sgbm_backend(
    const CpuSgbmConfig& config) {
  return std::make_unique<CpuSgbmBackend>(config);
}

}  // namespace depthd
