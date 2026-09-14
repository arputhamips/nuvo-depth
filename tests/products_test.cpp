#include "depthd/products.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

template <typename Function>
bool throws(Function&& function) {
  try {
    function();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

int fail(const int code, const std::string_view message) {
  std::cerr << "products_test: " << message << '\n';
  return code;
}

}  // namespace

int main() {
  constexpr std::string_view digest =
      "0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef";
  const auto identity = depthd::calibration_identity_from_sha256(digest);
  if (!depthd::is_present(identity) ||
      depthd::calibration_identity_to_sha256(identity) != digest) {
    return fail(1, "calibration identity did not round-trip");
  }
  if (!throws([] {
        (void)depthd::calibration_identity_from_sha256("not-a-digest");
      })) {
    return fail(2, "malformed calibration identity was accepted");
  }

  const auto image_grid = depthd::make_reference_grid(728U, 544U);
  const auto native_grid = depthd::make_resampled_grid(image_grid, 364U, 272U);
  if (native_grid.x_scale_to_reference != 2.0 ||
      native_grid.y_scale_to_reference != 2.0 ||
      native_grid.x_offset_to_reference != 0.5 ||
      native_grid.y_offset_to_reference != 0.5) {
    return fail(3, "native grid does not describe pixel-center resampling");
  }

  const std::array<double, 12> left_projection{
      493.542046188, 0.0, 348.240810394, 0.0,
      0.0, 493.542046188, 266.509811401, 0.0,
      0.0, 0.0, 1.0, 0.0};
  const std::array<double, 12> right_projection{
      493.542046188, 0.0, 348.240810394, -49.3542046188,
      0.0, 493.542046188, 266.509811401, 0.0,
      0.0, 0.0, 1.0, 0.0};
  const auto calibration = depthd::make_rectified_calibration(
      identity,
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired,
      image_grid, left_projection, right_projection);
  if (std::abs(calibration.baseline_m - 0.1) > 1e-12 ||
      depthd::metric_depth_authorized(calibration.authority)) {
    return fail(4, "provisional seed was treated as validated metric calibration");
  }
  if (!depthd::metric_depth_authorized(
          depthd::CalibrationAuthority::kFactoryValidated) ||
      !depthd::metric_depth_authorized(
          depthd::CalibrationAuthority::kUserValidated)) {
    return fail(5, "validated calibration authority was not recognized");
  }

  const auto native_calibration =
      depthd::resample_rectified_calibration(calibration, native_grid);
  if (!depthd::same_grid(native_calibration.grid, native_grid) ||
      std::abs(native_calibration.left_projection[0] -
               left_projection[0] / 2.0) > 1e-12 ||
      std::abs(native_calibration.right_projection[3] -
               right_projection[3] / 2.0) > 1e-12 ||
      std::abs(native_calibration.baseline_m - calibration.baseline_m) > 1e-12) {
    return fail(6, "resampled calibration does not match the native pixel grid");
  }

  depthd::ProductSetMetadata metadata;
  metadata.frame.device_uuid[0] = 1U;
  metadata.frame.run_uuid[0] = 2U;
  metadata.frame.capture_epoch = 1U;
  metadata.frame.capture_sequence = 42U;
  metadata.frame.configuration_epoch = 3U;
  metadata.frame.capture_time_ns = 8'000'000'000LL;
  metadata.frame.capture_clock = depthd::CaptureClock::kBoottime;
  metadata.calibration = identity;
  metadata.calibration_authority = calibration.authority;
  metadata.left_rectified = {image_grid, {0U, 0U, 728U, 544U},
                             depthd::CameraRole::kLogicalLeft,
                             depthd::ImageProductFormat::kGray8};
  metadata.right_rectified = {image_grid, {0U, 0U, 728U, 544U},
                              depthd::CameraRole::kLogicalRight,
                              depthd::ImageProductFormat::kGray8};
  metadata.disparity.grid = native_grid;
  metadata.disparity.valid_roi = {64U, 0U, 300U, 272U};
  metadata.disparity.accepted_min_disparity_px = 0.0F;
  metadata.disparity.accepted_max_disparity_px = 64.0F;
  metadata.disparity.algorithmic_quantization_px = 1.0F / 16.0F;
  metadata.disparity.confidence_model =
      depthd::ConfidenceModel::kBinarySgbmSupportV1;
  metadata.disparity.derivation =
      depthd::DisparityDerivation::kNativeMatcherGrid;
  metadata.depth.grid = native_grid;
  metadata.depth.minimum_depth_m = 0.3F;
  metadata.depth.maximum_depth_m = 20.0F;
  try {
    depthd::validate(metadata, calibration, native_calibration);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return fail(7, "valid associated product metadata was rejected");
  }
  const auto compatibility_metadata =
      depthd::make_resampled_disparity_metadata(metadata.disparity, image_grid);
  if (compatibility_metadata.derivation !=
          depthd::DisparityDerivation::
              kInvalidAwareBilinearCompatibility ||
      compatibility_metadata.valid_roi.x != 129U ||
      compatibility_metadata.valid_roi.width != 599U ||
      compatibility_metadata.accepted_max_disparity_px != 128.0F ||
      compatibility_metadata.algorithmic_quantization_px != 1.0F / 8.0F) {
    return fail(8, "derived disparity metadata hides its grid or quantization");
  }

  const auto image_pixels = static_cast<std::size_t>(image_grid.width) *
                            image_grid.height;
  const auto native_pixels = static_cast<std::size_t>(native_grid.width) *
                             native_grid.height;
  std::vector<std::uint8_t> left_image(image_pixels);
  std::vector<std::uint8_t> right_image(image_pixels);
  std::vector<float> product_disparity(native_pixels);
  std::vector<float> product_depth(native_pixels);
  std::vector<std::uint8_t> product_confidence(native_pixels);
  std::vector<std::uint8_t> product_validity(native_pixels);
  depthd::ProductSetView product_set{
      metadata,
      {left_image.data(), image_grid, image_grid.width,
       depthd::ImageProductFormat::kGray8},
      {right_image.data(), image_grid, image_grid.width,
       depthd::ImageProductFormat::kGray8},
      {product_disparity.data(), native_grid, native_grid.width},
      {product_depth.data(), native_grid, native_grid.width},
      {product_confidence.data(), native_grid, native_grid.width},
      {product_validity.data(), native_grid, native_grid.width}};
  try {
    depthd::validate(product_set);
  } catch (const std::exception&) {
    return fail(9, "associated product views were rejected");
  }
  auto mismatched_product_set = product_set;
  mismatched_product_set.confidence.grid = image_grid;
  if (!throws([&] { depthd::validate(mismatched_product_set); })) {
    return fail(10, "product view accepted a plane on the wrong grid");
  }

  auto bad_metadata = metadata;
  bad_metadata.frame.capture_sequence = 0U;
  if (!throws([&] { depthd::validate(bad_metadata); })) {
    return fail(11, "unassociated product metadata was accepted");
  }
  bad_metadata = metadata;
  bad_metadata.disparity.valid_roi.width = 301U;
  if (!throws([&] { depthd::validate(bad_metadata); })) {
    return fail(12, "out-of-grid valid ROI was accepted");
  }
  bad_metadata = metadata;
  bad_metadata.calibration_authority =
      depthd::CalibrationAuthority::kFactoryValidated;
  if (!throws(
          [&] { depthd::validate(bad_metadata, calibration, native_calibration); })) {
    return fail(13, "product metadata accepted a false calibration authority");
  }

  const auto small_grid = depthd::make_reference_grid(4U, 3U);
  if (depthd::required_bytes(small_grid, 6U, sizeof(float)) != 64U) {
    return fail(14, "padded plane byte calculation is incorrect");
  }
  if (!throws([&] {
        (void)depthd::required_bytes(
            small_grid, std::numeric_limits<std::size_t>::max(),
            sizeof(float));
      })) {
    return fail(15, "overflowing plane layout was accepted");
  }

  std::array<float, 12> disparity{};
  std::array<std::uint8_t, 12> confidence{};
  std::array<std::uint8_t, 12> validity{};
  depthd::MutableDisparityProductsView views;
  views.disparity = {disparity.data(), small_grid, 4U};
  views.confidence = {confidence.data(), small_grid, 4U};
  views.validity = {validity.data(), small_grid, 4U};
  try {
    depthd::validate(views);
  } catch (const std::exception&) {
    return fail(16, "valid product plane layout was rejected");
  }
  views.validity.grid = depthd::make_reference_grid(3U, 3U);
  if (!throws([&] { depthd::validate(views); })) {
    return fail(17, "mismatched product grids were accepted");
  }

  auto corrupted_calibration = calibration;
  corrupted_calibration.reprojection_q[15] += 1.0;
  if (!throws([&] { depthd::validate(corrupted_calibration); })) {
    return fail(18, "inconsistent Q matrix was accepted");
  }
  return 0;
}
