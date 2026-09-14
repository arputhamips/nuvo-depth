#include "depthd/stereo_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace depthd {
namespace {

using Clock = std::chrono::steady_clock;

struct StageTimes {
  Clock::time_point started{};
  Clock::time_point preprocessed{};
  Clock::time_point matched{};
};

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

[[nodiscard]] constexpr std::uint8_t code(const ValidityCode value) noexcept {
  return static_cast<std::uint8_t>(value);
}

void validate_config(const CpuSgbmConfig& config) {
  constexpr auto maximum_dimension = 16'384U;
  if (config.input_width == 0U || config.input_height == 0U ||
      config.working_width == 0U || config.working_height == 0U ||
      config.input_width > maximum_dimension ||
      config.input_height > maximum_dimension ||
      config.working_width > maximum_dimension ||
      config.working_height > maximum_dimension) {
    throw std::invalid_argument("SGBM image dimensions must be in 1..16384");
  }
  if (config.working_width > config.input_width ||
      config.working_height > config.input_height) {
    throw std::invalid_argument("SGBM working dimensions cannot exceed input");
  }
  const auto horizontal_ratio = static_cast<std::uint64_t>(config.input_width) *
                                config.working_height;
  const auto vertical_ratio = static_cast<std::uint64_t>(config.input_height) *
                              config.working_width;
  if (horizontal_ratio != vertical_ratio) {
    throw std::invalid_argument(
        "SGBM input and working grids must have equal aspect ratio");
  }
  if (config.working_disparities <= 0 ||
      config.working_disparities % 16 != 0 ||
      config.working_disparities >=
          static_cast<std::int32_t>(config.working_width)) {
    throw std::invalid_argument(
        "SGBM working disparity count must be a positive multiple of 16 "
        "smaller than the working width");
  }
  if (!std::isfinite(config.accepted_min_disparity_px) ||
      !std::isfinite(config.accepted_max_disparity_px) ||
      config.accepted_max_disparity_px <=
          config.accepted_min_disparity_px ||
      config.block_size < 1 ||
      config.block_size > 255 || config.block_size % 2 == 0) {
    throw std::invalid_argument("invalid SGBM disparity range or block size");
  }
  if (config.opencv_threads < 1 || config.opencv_threads > 64) {
    throw std::invalid_argument("OpenCV thread count must be in 1..64");
  }
  if (config.algorithm == CpuSgbmConfig::Algorithm::kBm &&
      config.block_size < 5) {
    throw std::invalid_argument("StereoBM block size must be at least 5");
  }
  if (config.uniqueness_ratio < 0 || config.uniqueness_ratio > 100 ||
      config.speckle_window_size < 0 || config.speckle_window_size > 10000 ||
      config.speckle_range < 0 || config.speckle_range > 256 ||
      config.disparity_12_max_diff < -1 ||
      config.disparity_12_max_diff > 256 ||
      config.pre_filter_cap < 1 || config.pre_filter_cap > 63 ||
      config.bm_pre_filter_size < 5 || config.bm_pre_filter_size > 255 ||
      config.bm_pre_filter_size % 2 == 0 ||
      config.bm_texture_threshold < 0 ||
      config.bm_texture_threshold > 255 ||
      config.specular_threshold < 1 || config.specular_threshold > 256) {
    throw std::invalid_argument("stereo matcher control is out of range");
  }
  const auto search_end =
      static_cast<std::int64_t>(config.working_min_disparity) +
      config.working_disparities;
  if (search_end <= 0 ||
      search_end >= static_cast<std::int64_t>(config.working_width)) {
    throw std::invalid_argument(
        "SGBM search interval leaves no non-border output region");
  }
  const auto invalid_sentinel_fixed =
      (static_cast<std::int64_t>(config.working_min_disparity) - 1) * 16;
  const auto search_end_fixed = search_end * 16;
  if (invalid_sentinel_fixed < std::numeric_limits<std::int16_t>::min() ||
      search_end_fixed > std::numeric_limits<std::int16_t>::max()) {
    throw std::invalid_argument(
        "SGBM search interval exceeds OpenCV fixed-disparity storage");
  }
  if (config.accepted_min_disparity_px <
          static_cast<float>(config.working_min_disparity) ||
      config.accepted_max_disparity_px > static_cast<float>(search_end)) {
    throw std::invalid_argument(
        "accepted full-resolution disparity range is inconsistent with the "
        "native matcher search interval");
  }
}

[[nodiscard]] CpuSgbmConfig load_control_file(
    const std::string& path, const CpuSgbmConfig& base) {
  cv::FileStorage storage(path, cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON);
  if (!storage.isOpened() || !storage.root().isMap()) {
    throw std::invalid_argument("stereo control file is not a JSON object");
  }
  const auto root = storage.root();
  const auto required_int = [&](const char* key) {
    const auto node = root[key];
    if (node.empty() || !node.isInt()) {
      throw std::invalid_argument(std::string("invalid stereo field: ") + key);
    }
    return static_cast<int>(node);
  };
  if (required_int("schema_version") != 1) {
    throw std::invalid_argument("unsupported stereo control schema");
  }
  const auto algorithm_node = root["algorithm"];
  if (algorithm_node.empty() || !algorithm_node.isString()) {
    throw std::invalid_argument("invalid stereo algorithm");
  }
  const auto algorithm = static_cast<std::string>(algorithm_node);
  CpuSgbmConfig updated = base;
  if (algorithm == "sgbm") updated.algorithm = CpuSgbmConfig::Algorithm::kSgbm;
  else if (algorithm == "bm") updated.algorithm = CpuSgbmConfig::Algorithm::kBm;
  else throw std::invalid_argument("unknown stereo algorithm");
  updated.working_min_disparity = required_int("min_disparity");
  updated.working_disparities = required_int("search_range");
  updated.accepted_min_disparity_px =
      static_cast<float>(updated.working_min_disparity);
  updated.accepted_max_disparity_px =
      static_cast<float>(required_int("max_disparity"));
  updated.block_size = required_int("block_size");
  updated.uniqueness_ratio = required_int("uniqueness_ratio");
  updated.speckle_window_size = required_int("speckle_window_size");
  updated.speckle_range = required_int("speckle_range");
  updated.disparity_12_max_diff = required_int("disparity_12_max_diff");
  updated.pre_filter_cap = required_int("pre_filter_cap");
  updated.bm_pre_filter_size = required_int("bm_pre_filter_size");
  updated.bm_texture_threshold = required_int("bm_texture_threshold");
  updated.specular_threshold = required_int("specular_threshold");
  updated.opencv_threads = required_int("opencv_threads");
  validate_config(updated);
  return updated;
}

class CpuSgbmBackend final : public StereoBackend,
                             public NativeDisparityBackend {
 public:
  explicit CpuSgbmBackend(const CpuSgbmConfig& config)
      : base_config_(config), config_(config) {
    validate_config(config_);
    input_grid_ = make_reference_grid(config_.input_width, config_.input_height);
    native_grid_ = make_resampled_grid(input_grid_, config_.working_width,
                                       config_.working_height);
    disparity_scale_ = static_cast<float>(native_grid_.x_scale_to_reference);
    rebuild_matcher();

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
    confidence_working_.create(working_size, CV_8UC1);
    validity_working_.create(working_size, CV_8UC1);
    valid_pixels_by_row_.resize(config_.working_height);
    legacy_confidence_.create(input_size, CV_8UC1);
    legacy_validity_.create(input_size, CV_8UC1);
    refresh_configuration();
  }

  void refresh_configuration() override {
    if (base_config_.control_file.empty()) return;
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(
        base_config_.control_file, error);
    if (error || (control_time_ && modified == *control_time_)) return;
    control_time_ = modified;
    try {
      auto updated = load_control_file(base_config_.control_file, base_config_);
      const auto previous = config_;
      const auto previous_metadata = metadata_;
      const auto previous_matcher = matcher_;
      config_ = std::move(updated);
      try {
        rebuild_matcher();
      } catch (...) {
        config_ = previous;
        metadata_ = previous_metadata;
        matcher_ = previous_matcher;
        cv::setNumThreads(previous.opencv_threads);
        throw;
      }
    } catch (const std::exception& failure) {
      std::cerr << "depthd: stereo control update ignored: "
                << failure.what() << '\n';
    }
  }

  [[nodiscard]] std::string_view name() const noexcept override {
    return config_.algorithm == CpuSgbmConfig::Algorithm::kBm
               ? "opencv_bm_cpu" : "opencv_sgbm_cpu";
  }

  [[nodiscard]] DisparityMetadata metadata() const noexcept override {
    return metadata_;
  }

 private:
  void rebuild_matcher() {
    cv::setNumThreads(config_.opencv_threads);
    metadata_.grid = native_grid_;
    const cv::Rect matcher_roi = cv::getValidDisparityROI(
        cv::Rect(0, 0, static_cast<int>(config_.working_width),
                 static_cast<int>(config_.working_height)),
        cv::Rect(0, 0, static_cast<int>(config_.working_width),
                 static_cast<int>(config_.working_height)),
        config_.working_min_disparity, config_.working_disparities,
        config_.block_size);
    if (matcher_roi.width <= 0 || matcher_roi.height <= 0) {
      throw std::invalid_argument("SGBM search has no valid matcher ROI");
    }
    metadata_.valid_roi = {
        static_cast<std::uint32_t>(matcher_roi.x),
        static_cast<std::uint32_t>(matcher_roi.y),
        static_cast<std::uint32_t>(matcher_roi.width),
        static_cast<std::uint32_t>(matcher_roi.height)};
    metadata_.accepted_min_disparity_px =
        config_.accepted_min_disparity_px;
    metadata_.accepted_max_disparity_px =
        config_.accepted_max_disparity_px;
    metadata_.algorithmic_quantization_px = 1.0F / 16.0F;
    metadata_.confidence_model =
        config_.algorithm == CpuSgbmConfig::Algorithm::kBm
            ? ConfidenceModel::kBinaryBmSupportV1
            : ConfidenceModel::kBinarySgbmSupportV1;
    metadata_.derivation = DisparityDerivation::kNativeMatcherGrid;
    validate(metadata_);

    if (config_.algorithm == CpuSgbmConfig::Algorithm::kBm) {
      auto bm = cv::StereoBM::create(config_.working_disparities,
                                      config_.block_size);
      bm->setMinDisparity(config_.working_min_disparity);
      bm->setPreFilterSize(config_.bm_pre_filter_size);
      bm->setPreFilterCap(config_.pre_filter_cap);
      bm->setTextureThreshold(config_.bm_texture_threshold);
      bm->setUniquenessRatio(config_.uniqueness_ratio);
      bm->setSpeckleWindowSize(config_.speckle_window_size);
      bm->setSpeckleRange(config_.speckle_range);
      bm->setDisp12MaxDiff(config_.disparity_12_max_diff);
      matcher_ = bm;
    } else {
      const int window_area = config_.block_size * config_.block_size;
      matcher_ = cv::StereoSGBM::create(
          config_.working_min_disparity, config_.working_disparities,
          config_.block_size, 8 * window_area, 32 * window_area,
          config_.disparity_12_max_diff, config_.pre_filter_cap,
          config_.uniqueness_ratio, config_.speckle_window_size,
          config_.speckle_range, cv::StereoSGBM::MODE_SGBM_3WAY);
    }
  }

 public:

  void process_native(const ImageView& left, const ImageView& right,
                      const MutableDisparityProductsView& output,
                      StereoBackendStats& stats) override {
    validate_input_views(left, right);
    validate(output);
    if (!same_grid(output.disparity.grid, native_grid_)) {
      throw std::invalid_argument(
          "native disparity products must use the matcher grid");
    }

    const auto stages = compute_match(left, right);
    const auto valid_pixels = classify_native_disparity(output);
    finish_native_stats(stages, Clock::now(), valid_pixels, stats);
  }

  void process(const ImageView& left, const ImageView& right,
               const MutableDisparityView& output,
               StereoBackendStats& stats) override {
    refresh_configuration();
    validate_input_views(left, right);
    validate_legacy_output(output);
    const auto stages = compute_match(left, right);

    const MutableDisparityProductsView native_scratch{
        {disparity_working_.ptr<float>(), native_grid_,
         disparity_working_.step1()},
        {confidence_working_.ptr<std::uint8_t>(), native_grid_,
         confidence_working_.step1()},
        {validity_working_.ptr<std::uint8_t>(), native_grid_,
         validity_working_.step1()}};
    (void)classify_native_disparity(native_scratch);

    const FloatPlaneView source_disparity{
        disparity_working_.ptr<float>(), native_grid_,
        disparity_working_.step1()};
    const BytePlaneView source_confidence{
        confidence_working_.ptr<std::uint8_t>(), native_grid_,
        confidence_working_.step1()};
    const BytePlaneView source_validity{
        validity_working_.ptr<std::uint8_t>(), native_grid_,
        validity_working_.step1()};
    MutableDisparityProductsView target;
    target.disparity = {output.data, input_grid_, output.stride_elements};
    target.confidence = {legacy_confidence_.ptr<std::uint8_t>(), input_grid_,
                         legacy_confidence_.step1()};
    target.validity = {legacy_validity_.ptr<std::uint8_t>(), input_grid_,
                       legacy_validity_.step1()};
    resample_disparity_invalid_aware(
        source_disparity, source_confidence, source_validity, target);

    const auto legacy_metadata =
        make_resampled_disparity_metadata(metadata_, input_grid_);
    finish_stats(stages, Clock::now(), legacy_validity_,
                 legacy_metadata.valid_roi, stats);
  }

 private:
  [[nodiscard]] StageTimes compute_match(const ImageView& left,
                                         const ImageView& right) {
    StageTimes stages;
    stages.started = Clock::now();
    prepare_working(left, left_gray_, left_working_);
    prepare_working(right, right_gray_, right_working_);
    stages.preprocessed = Clock::now();

    matcher_->compute(left_working_, right_working_, disparity_fixed_);
    stages.matched = Clock::now();
    return stages;
  }

  [[nodiscard]] std::size_t classify_native_disparity(
      const MutableDisparityProductsView& output) {
    const auto invalid_value = std::numeric_limits<float>::quiet_NaN();
    const auto width = disparity_fixed_.cols;
    const auto roi_left = static_cast<int>(metadata_.valid_roi.x);
    const auto roi_right = static_cast<int>(metadata_.valid_roi.x +
                                            metadata_.valid_roi.width);
    const auto roi_top = static_cast<int>(metadata_.valid_roi.y);
    const auto roi_bottom = static_cast<int>(metadata_.valid_roi.y +
                                             metadata_.valid_roi.height);
    const auto minimum = metadata_.accepted_min_disparity_px;
    const auto maximum = metadata_.accepted_max_disparity_px;
    const auto outside_roi = code(ValidityCode::kOutsideRoi);

    // Rows are independent. OpenCV's configured worker pool is already warm
    // from SGBM, so reuse it and give every worker a disjoint reduction slot.
    cv::parallel_for_(cv::Range(0, disparity_fixed_.rows),
                      [&](const cv::Range& rows) {
      for (int y = rows.start; y < rows.end; ++y) {
        const auto* fixed = disparity_fixed_.ptr<std::int16_t>(y);
        auto* disparity = output.disparity.data +
                          static_cast<std::size_t>(y) *
                              output.disparity.stride_elements;
        auto* confidence = output.confidence.data +
                           static_cast<std::size_t>(y) *
                               output.confidence.stride_elements;
        auto* validity = output.validity.data +
                         static_cast<std::size_t>(y) *
                             output.validity.stride_elements;
        std::size_t row_valid = 0U;
        if (y < roi_top || y >= roi_bottom) {
          std::fill_n(disparity, width, invalid_value);
          std::fill_n(confidence, width, 0U);
          std::fill_n(validity, width, outside_roi);
          valid_pixels_by_row_[static_cast<std::size_t>(y)] = 0U;
          continue;
        }

        std::fill_n(disparity, roi_left, invalid_value);
        std::fill_n(confidence, roi_left, 0U);
        std::fill_n(validity, roi_left, outside_roi);
        for (int x = roi_left; x < roi_right; ++x) {
          const auto disparity_px = static_cast<float>(fixed[x]) / 16.0F;
          const auto classification =
              disparity_px < minimum
                  ? ValidityCode::kInconsistent
                  : disparity_px >= maximum
                        ? ValidityCode::kOutsideDisparityRange
                        : left_working_.ptr<std::uint8_t>(y)[x] >=
                                  config_.specular_threshold
                              ? ValidityCode::kInconsistent
                              : ValidityCode::kValid;
          const auto is_valid = classification == ValidityCode::kValid;
          row_valid += is_valid ? 1U : 0U;
          disparity[x] = is_valid ? disparity_px : invalid_value;
          confidence[x] = is_valid ? 255U : 0U;
          validity[x] = code(classification);
        }
        const auto suffix = width - roi_right;
        std::fill_n(disparity + roi_right, suffix, invalid_value);
        std::fill_n(confidence + roi_right, suffix, 0U);
        std::fill_n(validity + roi_right, suffix, outside_roi);
        valid_pixels_by_row_[static_cast<std::size_t>(y)] = row_valid;
      }
    });
    return std::accumulate(valid_pixels_by_row_.begin(),
                           valid_pixels_by_row_.end(), std::size_t{0U});
  }

  void finish_native_stats(const StageTimes& stages,
                           const Clock::time_point finished,
                           const std::size_t valid_pixels,
                           StereoBackendStats& stats) const {
    stats.preprocess_ms = elapsed_ms(stages.started, stages.preprocessed);
    stats.match_ms = elapsed_ms(stages.preprocessed, stages.matched);
    stats.postprocess_ms = elapsed_ms(stages.matched, finished);
    stats.total_ms = elapsed_ms(stages.started, finished);
    stats.valid_fraction = static_cast<float>(valid_pixels) /
                           static_cast<float>(native_grid_.width *
                                              native_grid_.height);
    stats.searchable_valid_fraction =
        static_cast<float>(valid_pixels) /
        static_cast<float>(metadata_.valid_roi.width *
                           metadata_.valid_roi.height);
  }

  static void finish_stats(const StageTimes& stages,
                           const Clock::time_point finished,
                           const cv::Mat& validity, const RegionU32& valid_roi,
                           StereoBackendStats& stats) {
    std::size_t valid_pixels = 0U;
    std::size_t searchable_valid_pixels = 0U;
    for (int y = 0; y < validity.rows; ++y) {
      const auto* row = validity.ptr<std::uint8_t>(y);
      for (int x = 0; x < validity.cols; ++x) {
        if (row[x] == code(ValidityCode::kValid)) {
          ++valid_pixels;
          if (x >= static_cast<int>(valid_roi.x) &&
              x < static_cast<int>(valid_roi.x + valid_roi.width) &&
              y >= static_cast<int>(valid_roi.y) &&
              y < static_cast<int>(valid_roi.y + valid_roi.height)) {
            ++searchable_valid_pixels;
          }
        }
      }
    }
    stats.preprocess_ms = elapsed_ms(stages.started, stages.preprocessed);
    stats.match_ms = elapsed_ms(stages.preprocessed, stages.matched);
    stats.postprocess_ms = elapsed_ms(stages.matched, finished);
    stats.total_ms = elapsed_ms(stages.started, finished);
    stats.valid_fraction = static_cast<float>(valid_pixels) /
                           static_cast<float>(validity.total());
    stats.searchable_valid_fraction =
        static_cast<float>(searchable_valid_pixels) /
        static_cast<float>(valid_roi.width * valid_roi.height);
  }

  void validate_input_views(const ImageView& left,
                            const ImageView& right) const {
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
  }

  void validate_legacy_output(const MutableDisparityView& output) const {
    if (output.data == nullptr || output.width != config_.input_width ||
        output.height != config_.input_height ||
        output.stride_elements < output.width) {
      throw std::invalid_argument("disparity output geometry is invalid");
    }
  }

  static void prepare_working(const ImageView& view, cv::Mat& gray_scratch,
                              cv::Mat& working) {
    const cv::Mat source(static_cast<int>(view.height),
                         static_cast<int>(view.width), cv_type(view.encoding),
                         const_cast<std::uint8_t*>(view.data), view.stride_bytes);
    switch (view.encoding) {
      case ImageEncoding::kGray8:
        if (source.size() == working.size()) source.copyTo(working);
        else cv::resize(source, working, working.size(), 0.0, 0.0,
                        cv::INTER_AREA);
        break;
      case ImageEncoding::kBgr8:
        cv::cvtColor(source, gray_scratch, cv::COLOR_BGR2GRAY);
        if (gray_scratch.size() == working.size()) gray_scratch.copyTo(working);
        else cv::resize(gray_scratch, working, working.size(), 0.0, 0.0,
                        cv::INTER_AREA);
        break;
      case ImageEncoding::kRgb8:
        cv::cvtColor(source, gray_scratch, cv::COLOR_RGB2GRAY);
        if (gray_scratch.size() == working.size()) gray_scratch.copyTo(working);
        else cv::resize(gray_scratch, working, working.size(), 0.0, 0.0,
                        cv::INTER_AREA);
        break;
    }
  }

  CpuSgbmConfig base_config_;
  CpuSgbmConfig config_;
  std::optional<std::filesystem::file_time_type> control_time_;
  PixelGrid input_grid_{};
  PixelGrid native_grid_{};
  DisparityMetadata metadata_{};
  float disparity_scale_{};
  cv::Ptr<cv::StereoMatcher> matcher_;
  cv::Mat left_gray_;
  cv::Mat right_gray_;
  cv::Mat left_working_;
  cv::Mat right_working_;
  cv::Mat disparity_fixed_;
  cv::Mat disparity_working_;
  cv::Mat confidence_working_;
  cv::Mat validity_working_;
  std::vector<std::size_t> valid_pixels_by_row_;
  cv::Mat legacy_confidence_;
  cv::Mat legacy_validity_;
};

}  // namespace

std::unique_ptr<StereoBackend> make_cpu_sgbm_backend(
    const CpuSgbmConfig& config) {
  return std::make_unique<CpuSgbmBackend>(config);
}

std::unique_ptr<NativeDisparityBackend> make_cpu_sgbm_native_backend(
    const CpuSgbmConfig& config) {
  return std::make_unique<CpuSgbmBackend>(config);
}

}  // namespace depthd
