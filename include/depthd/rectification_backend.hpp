#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "depthd/stereo_backend.hpp"

namespace depthd {

struct MutableImageView {
  std::uint8_t* data{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t stride_bytes{};
  ImageEncoding encoding{ImageEncoding::kGray8};
};

// Dense inverse-remap coordinates in source-image pixel space. Each output
// pixel samples source_x[index], source_y[index]. The maps are generated once
// from the calibrated camera model and are immutable during steady state.
struct RectificationMap {
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<float> source_x;
  std::vector<float> source_y;
};

struct StereoRectificationMaps {
  RectificationMap left;
  RectificationMap right;
};

enum class RectificationCamera : std::uint8_t {
  kLeft,
  kRight,
};

enum class RectificationInterpolation : std::uint8_t {
  kLinear,
};

struct RectificationBackendConfig {
  std::uint32_t input_width{728};
  std::uint32_t input_height{544};
  std::uint32_t output_width{728};
  std::uint32_t output_height{544};
  RectificationInterpolation interpolation{RectificationInterpolation::kLinear};
  std::uint8_t border_value{0};
};

struct RectificationStats {
  double input_transfer_ms{};
  double compute_ms{};
  double output_transfer_ms{};
  double total_ms{};
};

class RectificationBackend {
 public:
  virtual ~RectificationBackend() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  virtual void rectify(RectificationCamera camera, const ImageView& input,
                       const MutableImageView& output,
                       RectificationStats& stats) = 0;

  // Backends may override this to batch both cameras in one submission. The
  // default implementation calls rectify twice and combines the measurements.
  virtual void rectify_pair(const ImageView& left_input,
                            const ImageView& right_input,
                            const MutableImageView& left_output,
                            const MutableImageView& right_output,
                            RectificationStats& stats);
};

using RectificationBackendFactory = std::function<
    std::unique_ptr<RectificationBackend>(const RectificationBackendConfig&,
                                          const StereoRectificationMaps&)>;

// Runtime registry keeps optional implementations out of the common contract.
// A daemon registers only the modules compiled into its process, then selects
// the requested backend by configuration with an explicit fallback policy.
class RectificationBackendRegistry {
 public:
  void register_backend(std::string name,
                        RectificationBackendFactory factory);

  [[nodiscard]] bool contains(std::string_view name) const;

  [[nodiscard]] std::unique_ptr<RectificationBackend> create(
      std::string_view requested_backend, std::string_view fallback_backend,
      bool allow_fallback, const RectificationBackendConfig& config,
      const StereoRectificationMaps& maps) const;

 private:
  std::unordered_map<std::string, RectificationBackendFactory> factories_;
};

[[nodiscard]] bool is_valid(const RectificationMap& map) noexcept;
[[nodiscard]] bool is_valid(const StereoRectificationMaps& maps) noexcept;

}  // namespace depthd
