#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace depthd::ipc_v2 {

// Stable wire selections. The first six bits retain their original meaning.
enum class ProductKind : std::uint8_t {
  kLeftRectified = 0, kRightRectified = 1, kDisparity = 2, kDepth = 3,
  kConfidence = 4, kValidity = 5, kLeftMono = 6, kRightMono = 7,
  kLeftRgb = 8, kRightRgb = 9, kLeftRectifiedRgb = 10,
  kRightRectifiedRgb = 11, kRectifiedStereo = 12, kRectifiedStereoRgb = 13,
};
using ProductMask = std::uint32_t;
inline constexpr std::size_t kLegacyProductCount = 6U;
inline constexpr std::size_t kProductCount = 14U;
[[nodiscard]] constexpr ProductMask product_bit(const ProductKind kind) {
  const auto index = static_cast<std::uint8_t>(kind);
  return index < kProductCount ? 1U << index : 0U;
}
inline constexpr ProductMask kLegacyDefaultProducts = (1U << kLegacyProductCount) - 1U;
// Compatibility alias: this is the original six-product default, not all outputs.
inline constexpr ProductMask kAllProducts = kLegacyDefaultProducts;
inline constexpr ProductMask kSupportedProducts = (1U << kProductCount) - 1U;
inline constexpr ProductMask kMatcherProducts =
    product_bit(ProductKind::kDisparity) | product_bit(ProductKind::kDepth) |
    product_bit(ProductKind::kConfidence) | product_bit(ProductKind::kValidity);
inline constexpr ProductMask kImageProducts = kSupportedProducts & ~kMatcherProducts;
[[nodiscard]] constexpr std::string_view product_name(const ProductKind kind) {
  constexpr std::string_view names[]{"left_rectified", "right_rectified", "disparity",
      "depth", "confidence", "validity", "left_mono", "right_mono", "left_rgb",
      "right_rgb", "left_rectified_rgb", "right_rectified_rgb", "rectified_stereo",
      "rectified_stereo_rgb"};
  const auto index = static_cast<std::size_t>(kind);
  return index < kProductCount ? names[index] : std::string_view{};
}

}  // namespace depthd::ipc_v2
