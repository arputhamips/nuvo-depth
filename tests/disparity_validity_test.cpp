#include "depthd/products.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint8_t code(const depthd::ValidityCode value) {
  return static_cast<std::uint8_t>(value);
}

int fail(const int value, const std::string_view message) {
  std::cerr << "disparity_validity_test: " << message << '\n';
  return value;
}

}  // namespace

int main() {
  const auto target_grid = depthd::make_reference_grid(6U, 4U);
  const auto source_grid = depthd::make_resampled_grid(target_grid, 3U, 2U);
  const std::array<float, 6> source_disparity{1.0F, 2.0F, 3.0F,
                                               4.0F, 5.0F, 6.0F};
  std::array<std::uint8_t, 6> source_confidence{240U, 230U, 220U,
                                                 210U, 200U, 190U};
  std::array<std::uint8_t, 6> source_validity{};
  source_validity.fill(code(depthd::ValidityCode::kValid));

  constexpr std::size_t target_stride = 8U;
  std::vector<float> target_disparity(target_stride * target_grid.height,
                                      -123.0F);
  std::vector<std::uint8_t> target_confidence(
      target_stride * target_grid.height, 99U);
  std::vector<std::uint8_t> target_validity(target_stride * target_grid.height,
                                             99U);
  depthd::MutableDisparityProductsView target{
      {target_disparity.data(), target_grid, target_stride},
      {target_confidence.data(), target_grid, target_stride},
      {target_validity.data(), target_grid, target_stride}};
  depthd::resample_disparity_invalid_aware(
      {source_disparity.data(), source_grid, source_grid.width},
      {source_confidence.data(), source_grid, source_grid.width},
      {source_validity.data(), source_grid, source_grid.width}, target);

  const auto at = [&](const std::uint32_t x, const std::uint32_t y) {
    return target_disparity[static_cast<std::size_t>(y) * target_stride + x];
  };
  if (std::abs(at(0U, 0U) - 2.0F) > 1e-6F ||
      std::abs(at(1U, 1U) - 4.0F) > 1e-6F ||
      std::abs(at(5U, 3U) - 12.0F) > 1e-6F) {
    return fail(1, "valid bilinear compatibility values are incorrect");
  }
  if (target_confidence[target_stride + 1U] != 200U) {
    return fail(2, "derived confidence was not conservative");
  }
  for (std::uint32_t y = 0; y < target_grid.height; ++y) {
    if (target_disparity[static_cast<std::size_t>(y) * target_stride + 6U] !=
            -123.0F ||
        target_validity[static_cast<std::size_t>(y) * target_stride + 7U] !=
            99U) {
      return fail(3, "resampling modified target row padding");
    }
  }

  source_validity[1] = code(depthd::ValidityCode::kLowTexture);
  depthd::resample_disparity_invalid_aware(
      {source_disparity.data(), source_grid, source_grid.width},
      {source_confidence.data(), source_grid, source_grid.width},
      {source_validity.data(), source_grid, source_grid.width}, target);
  for (std::uint32_t y = 0; y < target_grid.height; ++y) {
    for (std::uint32_t x = 0; x < target_grid.width; ++x) {
      const bool influenced_by_invalid = y <= 2U && x >= 1U && x <= 4U;
      const auto index = static_cast<std::size_t>(y) * target_stride + x;
      if (influenced_by_invalid) {
        if (!std::isnan(target_disparity[index]) ||
            target_confidence[index] != 0U ||
            target_validity[index] !=
                code(depthd::ValidityCode::kLowTexture)) {
          return fail(4, "invalid source support contaminated a retained pixel");
        }
      } else if (!std::isfinite(target_disparity[index]) ||
                 target_validity[index] !=
                     code(depthd::ValidityCode::kValid)) {
        return fail(5, "unaffected target pixel was discarded");
      }
    }
  }

  auto nonfinite_source = source_disparity;
  source_validity.fill(code(depthd::ValidityCode::kValid));
  nonfinite_source[5] = std::numeric_limits<float>::quiet_NaN();
  depthd::resample_disparity_invalid_aware(
      {nonfinite_source.data(), source_grid, source_grid.width},
      {source_confidence.data(), source_grid, source_grid.width},
      {source_validity.data(), source_grid, source_grid.width}, target);
  const auto lower_right = 3U * target_stride + 5U;
  if (!std::isnan(target_disparity[lower_right]) ||
      target_validity[lower_right] !=
          code(depthd::ValidityCode::kNonFinite)) {
    return fail(6, "finite validity mask allowed NaN disparity contamination");
  }

  source_confidence[0] = 0U;
  depthd::resample_disparity_invalid_aware(
      {source_disparity.data(), source_grid, source_grid.width},
      {source_confidence.data(), source_grid, source_grid.width},
      {source_validity.data(), source_grid, source_grid.width}, target);
  if (!std::isnan(target_disparity[0]) ||
      target_validity[0] != code(depthd::ValidityCode::kInconsistent)) {
    return fail(7, "zero confidence was published as a valid disparity");
  }
  return 0;
}
