#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

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

struct CpuSgbmConfig {
  std::uint32_t input_width{728};
  std::uint32_t input_height{544};
  std::uint32_t working_width{364};
  std::uint32_t working_height{272};
  std::int32_t min_disparity{0};
  std::int32_t full_resolution_disparities{192};
  std::int32_t working_disparities{96};
  std::int32_t block_size{5};
  std::int32_t uniqueness_ratio{5};
  std::int32_t speckle_window_size{50};
  std::int32_t speckle_range{2};
  std::int32_t disparity_12_max_diff{1};
  std::int32_t pre_filter_cap{31};
  std::int32_t opencv_threads{1};
};

[[nodiscard]] std::unique_ptr<StereoBackend> make_cpu_sgbm_backend(
    const CpuSgbmConfig& config);

}  // namespace depthd
