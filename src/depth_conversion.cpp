#include "depthd/products.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace depthd {
namespace {

[[nodiscard]] constexpr std::uint8_t code(const ValidityCode value) noexcept {
  return static_cast<std::uint8_t>(value);
}

[[nodiscard]] constexpr std::uint8_t normalized_code(
    const std::uint8_t value) noexcept {
  return value <= code(ValidityCode::kQuantizationOverflow)
             ? value
             : code(ValidityCode::kUnknown);
}

void require_same_grid(const PixelGrid& left, const PixelGrid& right,
                       const char* message) {
  if (!same_grid(left, right)) {
    throw std::invalid_argument(message);
  }
}

struct HomogeneousDepth {
  double z{};
  double w{};
};

}  // namespace

DepthConversionStats convert_disparity_to_depth(
    const FloatPlaneView& disparity, const BytePlaneView& disparity_validity,
    const RectifiedCalibrationView& calibration,
    const DepthConversionOptions& options, const MutableFloatPlaneView& depth_m,
    const MutableBytePlaneView& depth_validity) {
  validate(disparity);
  validate(disparity_validity);
  validate(calibration);
  validate(depth_m);
  validate(depth_validity);
  require_same_grid(disparity.grid, disparity_validity.grid,
                    "disparity and validity must share one grid");
  require_same_grid(disparity.grid, calibration.grid,
                    "disparity grid must match the rectified calibration grid");
  require_same_grid(disparity.grid, depth_m.grid,
                    "depth output must match the disparity grid");
  require_same_grid(disparity.grid, depth_validity.grid,
                    "depth validity output must match the disparity grid");
  if (!std::isfinite(options.minimum_depth_m) ||
      !std::isfinite(options.maximum_depth_m) ||
      options.minimum_depth_m <= 0.0F ||
      options.maximum_depth_m <= options.minimum_depth_m) {
    throw std::invalid_argument(
        "depth limits must be finite, positive, and strictly ordered");
  }
  if (!metric_depth_authorized(calibration.authority) &&
      !options.allow_provisional_for_engineering) {
    throw std::invalid_argument(
        "provisional calibration requires an explicit engineering-only depth override");
  }

  DepthConversionStats stats;
  stats.pixel_count = static_cast<std::uint64_t>(disparity.grid.width) *
                      disparity.grid.height;
  const auto& q = calibration.reprojection_q;
  const auto invalid_depth = std::numeric_limits<float>::quiet_NaN();

  const auto convert_pixels = [&](auto&& project) {
    for (std::uint32_t y = 0; y < disparity.grid.height; ++y) {
      const auto* disparity_row =
          disparity.data + static_cast<std::size_t>(y) *
                               disparity.stride_elements;
      const auto* input_validity_row =
          disparity_validity.data +
          static_cast<std::size_t>(y) *
              disparity_validity.stride_elements;
      auto* depth_row = depth_m.data +
                        static_cast<std::size_t>(y) * depth_m.stride_elements;
      auto* output_validity_row =
          depth_validity.data +
          static_cast<std::size_t>(y) * depth_validity.stride_elements;
      for (std::uint32_t x = 0; x < disparity.grid.width; ++x) {
        const auto input_code = normalized_code(input_validity_row[x]);
        const auto disparity_px = disparity_row[x];

        if (input_code != code(ValidityCode::kValid)) {
          depth_row[x] = invalid_depth;
          output_validity_row[x] = input_code;
          ++stats.invalid_input_count;
          continue;
        }
        if (!std::isfinite(disparity_px)) {
          depth_row[x] = invalid_depth;
          output_validity_row[x] = code(ValidityCode::kNonFinite);
          ++stats.nonfinite_count;
          continue;
        }

        const auto homogeneous = project(x, y, disparity_px);
        if (!std::isfinite(homogeneous.z) ||
            !std::isfinite(homogeneous.w) || homogeneous.w <= 0.0) {
          depth_row[x] = invalid_depth;
          output_validity_row[x] =
              code(ValidityCode::kReprojectionFailure);
          ++stats.reprojection_failure_count;
          continue;
        }
        const auto depth = homogeneous.z / homogeneous.w;
        if (!std::isfinite(depth)) {
          depth_row[x] = invalid_depth;
          output_validity_row[x] =
              code(ValidityCode::kReprojectionFailure);
          ++stats.reprojection_failure_count;
          continue;
        }
        if (depth < static_cast<double>(options.minimum_depth_m) ||
            depth > static_cast<double>(options.maximum_depth_m)) {
          depth_row[x] = invalid_depth;
          output_validity_row[x] = code(ValidityCode::kOutsideDepthRange);
          ++stats.outside_depth_range_count;
          continue;
        }

        depth_row[x] = static_cast<float>(depth);
        output_validity_row[x] = code(ValidityCode::kValid);
        ++stats.valid_count;
      }
    }
  };

  const bool canonical_horizontal_q =
      q[8] == 0.0 && q[9] == 0.0 && q[10] == 0.0 && q[12] == 0.0 &&
      q[13] == 0.0;
  if (canonical_horizontal_q) {
    const auto z_numerator = q[11];
    const auto disparity_coefficient = q[14];
    const auto offset = q[15];
    convert_pixels([=](const std::uint32_t, const std::uint32_t,
                       const float disparity_px) {
      return HomogeneousDepth{
          z_numerator,
          disparity_coefficient * static_cast<double>(disparity_px) + offset};
    });
  } else {
    convert_pixels([&q](const std::uint32_t x, const std::uint32_t y,
                        const float disparity_px) {
      const auto x_value = static_cast<double>(x);
      const auto y_value = static_cast<double>(y);
      const auto disparity_value = static_cast<double>(disparity_px);
      return HomogeneousDepth{
          q[8] * x_value + q[9] * y_value + q[10] * disparity_value + q[11],
          q[12] * x_value + q[13] * y_value +
              q[14] * disparity_value + q[15]};
    });
  }
  return stats;
}

DepthQuantizationStats quantize_depth_to_u16_mm(
    const FloatPlaneView& depth_m, const BytePlaneView& depth_validity,
    const MutableU16PlaneView& depth_mm,
    const MutableBytePlaneView& quantized_validity) {
  validate(depth_m);
  validate(depth_validity);
  validate(depth_mm);
  validate(quantized_validity);
  require_same_grid(depth_m.grid, depth_validity.grid,
                    "depth and validity must share one grid");
  require_same_grid(depth_m.grid, depth_mm.grid,
                    "quantized depth must retain the depth grid");
  require_same_grid(depth_m.grid, quantized_validity.grid,
                    "quantized validity must retain the depth grid");

  DepthQuantizationStats stats;
  constexpr auto maximum =
      static_cast<double>(std::numeric_limits<std::uint16_t>::max());
  for (std::uint32_t y = 0; y < depth_m.grid.height; ++y) {
    for (std::uint32_t x = 0; x < depth_m.grid.width; ++x) {
      const auto input_index =
          static_cast<std::size_t>(y) * depth_m.stride_elements + x;
      const auto input_validity_index =
          static_cast<std::size_t>(y) * depth_validity.stride_elements + x;
      const auto output_index =
          static_cast<std::size_t>(y) * depth_mm.stride_elements + x;
      const auto output_validity_index =
          static_cast<std::size_t>(y) *
              quantized_validity.stride_elements +
          x;
      const auto input_code =
          normalized_code(depth_validity.data[input_validity_index]);
      const auto value = depth_m.data[input_index];
      if (input_code != code(ValidityCode::kValid)) {
        depth_mm.data[output_index] = kInvalidDepthMillimetres;
        quantized_validity.data[output_validity_index] = input_code;
        ++stats.invalid_input_count;
        continue;
      }

      const auto millimetres = static_cast<double>(value) * 1000.0;
      if (!std::isfinite(millimetres)) {
        depth_mm.data[output_index] = kInvalidDepthMillimetres;
        quantized_validity.data[output_validity_index] =
            code(ValidityCode::kNonFinite);
        ++stats.nonfinite_count;
        continue;
      }
      if (millimetres < 0.5 || millimetres > maximum) {
        depth_mm.data[output_index] = kInvalidDepthMillimetres;
        quantized_validity.data[output_validity_index] =
            code(ValidityCode::kQuantizationOverflow);
        ++stats.overflow_count;
        continue;
      }
      depth_mm.data[output_index] =
          static_cast<std::uint16_t>(std::lround(millimetres));
      quantized_validity.data[output_validity_index] =
          code(ValidityCode::kValid);
      ++stats.valid_count;
    }
  }
  return stats;
}

}  // namespace depthd
