#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "depthd/products.hpp"

namespace depthd {

enum class ImageEncoding : std::uint8_t {
  kGray8,
  kBgr8,
  kRgb8,
};

struct ImageView {
  const std::uint8_t* data{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t stride_bytes{};
  ImageEncoding encoding{ImageEncoding::kGray8};
};

struct MutableDisparityView {
  float* data{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t stride_elements{};
};

struct StereoBackendStats {
  double preprocess_ms{};
  double match_ms{};
  double postprocess_ms{};
  double total_ms{};
  float valid_fraction{};
  float searchable_valid_fraction{};
};

class StereoBackend {
 public:
  virtual ~StereoBackend() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  virtual void process(const ImageView& left, const ImageView& right,
                       const MutableDisparityView& output,
                       StereoBackendStats& stats) = 0;
};

// Native product interface for new runtime code. It intentionally lives beside
// the legacy full-image disparity interface instead of changing that vtable.
struct StereoConfigurationState {
  // Generation counts effective matcher changes, independently of formatting.
  std::uint64_t generation{1U};
  std::string requested_revision;
  std::string applied_revision;
  std::string rejection_error;
};

class NativeDisparityBackend {
 public:
  virtual ~NativeDisparityBackend() = default;

  // Called once at the start of each processing cycle, before metadata() is
  // used to allocate outputs. Backends may atomically apply runtime controls.
  virtual void refresh_configuration() {}
  [[nodiscard]] virtual StereoConfigurationState configuration_state() const {
    return {};
  }
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual DisparityMetadata metadata() const noexcept = 0;
  virtual void process_native(const ImageView& left, const ImageView& right,
                              const MutableDisparityProductsView& output,
                              StereoBackendStats& stats) = 0;
};

struct CpuSgbmConfig {
  enum class Algorithm { kSgbm, kBm };
  Algorithm algorithm{Algorithm::kSgbm};
  std::string control_file{};
  std::uint32_t input_width{728};
  std::uint32_t input_height{544};
  std::uint32_t working_width{364};
  std::uint32_t working_height{272};
  // Matcher-search units are pixels on the working grid. Accepted disparity
  // limits use the same grid explicitly; callers converting a full-resolution
  // configuration must scale all three values together.
  std::int32_t working_min_disparity{0};
  float accepted_min_disparity_px{0.0F};
  float accepted_max_disparity_px{64.0F};
  std::int32_t working_disparities{64};
  std::int32_t block_size{5};
  // OpenCV path aggregation: 3way (fastest), standard (5 directions),
  // hh4 (4 full paths), or hh (8 directions, highest memory/cost).
  std::string sgbm_mode{"3way"};
  std::int32_t uniqueness_ratio{1};
  std::int32_t speckle_window_size{30};
  std::int32_t speckle_range{2};
  std::int32_t disparity_12_max_diff{1};
  std::int32_t pre_filter_cap{31};
  std::int32_t bm_pre_filter_size{9};
  std::int32_t bm_texture_threshold{10};
  // 256 disables bright-pixel rejection; 1..255 rejects pixels at/above it.
  std::int32_t specular_threshold{256};
  // 0 disables smoothing. 3 and 5 median-filter already-valid subpixel
  // disparity while preserving the matcher's invalid mask.
  std::int32_t median_filter_size{0};
  std::int32_t opencv_threads{1};
  bool operator==(const CpuSgbmConfig&) const = default;
};

[[nodiscard]] std::unique_ptr<StereoBackend> make_cpu_sgbm_backend(
    const CpuSgbmConfig& config);
[[nodiscard]] std::unique_ptr<NativeDisparityBackend>
make_cpu_sgbm_native_backend(const CpuSgbmConfig& config);

}  // namespace depthd
