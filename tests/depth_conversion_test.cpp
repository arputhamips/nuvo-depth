#include "depthd/products.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::uint8_t code(const depthd::ValidityCode value) {
  return static_cast<std::uint8_t>(value);
}

int fail(const int value, const std::string_view message) {
  std::cerr << "depth_conversion_test: " << message << '\n';
  return value;
}

}  // namespace

int main() {
  const auto identity = depthd::calibration_identity_from_sha256(
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  const auto grid = depthd::make_reference_grid(6U, 1U);
  const std::array<double, 12> left_projection{
      100.0, 0.0, 2.0, 0.0,
      0.0, 100.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0};
  // cx differs by one pixel and -fx*baseline is -10 pixel-metres.
  const std::array<double, 12> right_projection{
      100.0, 0.0, 1.0, -10.0,
      0.0, 100.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0};
  const auto calibration = depthd::make_rectified_calibration(
      identity,
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired, grid,
      left_projection, right_projection);

  const std::array<float, 6> disparity{
      11.0F, 6.0F, 11.0F, std::numeric_limits<float>::quiet_NaN(), 1.0F,
      2.0F};
  const std::array<std::uint8_t, 6> disparity_validity{
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kLowTexture),
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kValid)};
  std::array<float, 6> depth{};
  std::array<std::uint8_t, 6> depth_validity{};
  try {
    (void)depthd::convert_disparity_to_depth(
        {disparity.data(), grid, grid.width},
        {disparity_validity.data(), grid, grid.width}, calibration, {},
        {depth.data(), grid, grid.width},
        {depth_validity.data(), grid, grid.width});
    return fail(1, "provisional calibration produced depth without an override");
  } catch (const std::invalid_argument&) {
  }
  const auto stats = depthd::convert_disparity_to_depth(
      {disparity.data(), grid, grid.width},
      {disparity_validity.data(), grid, grid.width}, calibration,
      {0.01F, 6.0F, true}, {depth.data(), grid, grid.width},
      {depth_validity.data(), grid, grid.width});
  if (std::abs(depth[0] - 1.0F) > 1e-6F ||
      std::abs(depth[1] - 2.0F) > 1e-6F) {
    return fail(2, "Q reprojection ignored units or principal-point offset");
  }
  if (!std::isnan(depth[2]) ||
      depth_validity[2] != code(depthd::ValidityCode::kLowTexture)) {
    return fail(3, "input validity classification was not preserved");
  }
  if (!std::isnan(depth[3]) ||
      depth_validity[3] != code(depthd::ValidityCode::kNonFinite)) {
    return fail(4, "nonfinite disparity was not classified");
  }
  if (!std::isnan(depth[4]) ||
      depth_validity[4] !=
          code(depthd::ValidityCode::kReprojectionFailure)) {
    return fail(5, "zero effective disparity was not rejected");
  }
  if (!std::isnan(depth[5]) ||
      depth_validity[5] != code(depthd::ValidityCode::kOutsideDepthRange)) {
    return fail(6, "depth limit was not classified");
  }
  if (stats.pixel_count != 6U || stats.valid_count != 2U ||
      stats.invalid_input_count != 1U || stats.nonfinite_count != 1U ||
      stats.reprojection_failure_count != 1U ||
      stats.outside_depth_range_count != 1U) {
    return fail(7, "depth conversion counters are inconsistent");
  }
  if (depthd::metric_depth_authorized(calibration.authority)) {
    return fail(8, "provisional calibration silently authorized metric depth");
  }

  const auto native_grid = depthd::make_resampled_grid(grid, 3U, 1U);
  const auto native_calibration =
      depthd::resample_rectified_calibration(calibration, native_grid);
  const std::array<float, 3> native_disparity{5.5F, 3.0F, 1.5F};
  const std::array<std::uint8_t, 3> native_validity{
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kValid)};
  std::array<float, 3> native_depth{};
  std::array<std::uint8_t, 3> native_depth_validity{};
  const auto native_stats = depthd::convert_disparity_to_depth(
      {native_disparity.data(), native_grid, native_grid.width},
      {native_validity.data(), native_grid, native_grid.width},
      native_calibration, {0.01F, 6.0F, true},
      {native_depth.data(), native_grid, native_grid.width},
      {native_depth_validity.data(), native_grid, native_grid.width});
  if (native_stats.valid_count != 3U ||
      std::abs(native_depth[0] - 1.0F) > 1e-6F ||
      std::abs(native_depth[1] - 2.0F) > 1e-6F ||
      std::abs(native_depth[2] - 5.0F) > 1e-6F) {
    return fail(9, "resampled calibration changed optical-axis depth");
  }

  const auto quantization_grid = depthd::make_reference_grid(5U, 1U);
  const std::array<float, 5> metres{
      1.2344F, 1.2346F, 70.0F,
      std::numeric_limits<float>::quiet_NaN(), 2.0F};
  const std::array<std::uint8_t, 5> metre_validity{
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kValid),
      code(depthd::ValidityCode::kInconsistent)};
  std::array<std::uint16_t, 5> millimetres{};
  std::array<std::uint8_t, 5> quantized_validity{};
  const auto quantization_stats = depthd::quantize_depth_to_u16_mm(
      {metres.data(), quantization_grid, quantization_grid.width},
      {metre_validity.data(), quantization_grid, quantization_grid.width},
      {millimetres.data(), quantization_grid, quantization_grid.width},
      {quantized_validity.data(), quantization_grid,
       quantization_grid.width});
  if (millimetres[0] != 1234U || millimetres[1] != 1235U ||
      millimetres[2] != depthd::kInvalidDepthMillimetres ||
      millimetres[3] != depthd::kInvalidDepthMillimetres ||
      millimetres[4] != depthd::kInvalidDepthMillimetres) {
    return fail(10, "millimetre quantization or invalid encoding is incorrect");
  }
  if (quantized_validity[2] !=
          code(depthd::ValidityCode::kQuantizationOverflow) ||
      quantized_validity[3] != code(depthd::ValidityCode::kNonFinite) ||
      quantized_validity[4] != code(depthd::ValidityCode::kInconsistent) ||
      quantization_stats.valid_count != 2U ||
      quantization_stats.overflow_count != 1U ||
      quantization_stats.nonfinite_count != 1U ||
      quantization_stats.invalid_input_count != 1U) {
    return fail(11, "quantization classification or counters are incorrect");
  }
  return 0;
}
