#include "depthd/products.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace depthd {
namespace {

constexpr std::uint32_t kMaximumGridDimension = 16'384;
constexpr double kComparisonTolerance = 1e-10;

[[nodiscard]] bool approximately_equal(const double left,
                                       const double right) noexcept {
  const auto scale = std::max({1.0, std::abs(left), std::abs(right)});
  return std::abs(left - right) <= kComparisonTolerance * scale;
}

void require_finite(const double value, const char* field) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(field) + " must be finite");
  }
}

[[nodiscard]] std::uint8_t hex_value(const char character) {
  if (character >= '0' && character <= '9') {
    return static_cast<std::uint8_t>(character - '0');
  }
  if (character >= 'a' && character <= 'f') {
    return static_cast<std::uint8_t>(character - 'a' + 10);
  }
  if (character >= 'A' && character <= 'F') {
    return static_cast<std::uint8_t>(character - 'A' + 10);
  }
  throw std::invalid_argument("calibration SHA-256 contains a non-hex character");
}

template <typename Plane>
void validate_plane(const Plane& plane, const std::size_t element_size,
                    const char* name) {
  if (plane.data == nullptr) {
    throw std::invalid_argument(std::string(name) + " data must not be null");
  }
  validate(plane.grid);
  if (plane.stride_elements < plane.grid.width) {
    throw std::invalid_argument(std::string(name) +
                                " stride must cover one complete row");
  }
  (void)required_bytes(plane.grid, plane.stride_elements, element_size);
}

void validate_projection(const std::array<double, 12>& projection,
                         const char* name) {
  for (const auto value : projection) {
    require_finite(value, name);
  }
  if (projection[0] <= 0.0 || projection[5] <= 0.0) {
    throw std::invalid_argument(std::string(name) +
                                " focal lengths must be positive");
  }
  if (!approximately_equal(projection[1], 0.0) ||
      !approximately_equal(projection[4], 0.0) ||
      !approximately_equal(projection[7], 0.0) ||
      !approximately_equal(projection[8], 0.0) ||
      !approximately_equal(projection[9], 0.0) ||
      !approximately_equal(projection[10], 1.0) ||
      !approximately_equal(projection[11], 0.0)) {
    throw std::invalid_argument(std::string(name) +
                                " is not a canonical rectified projection");
  }
}

[[nodiscard]] std::array<double, 16> make_q(
    const std::array<double, 12>& left,
    const std::array<double, 12>& right) {
  const auto translation_px_m = left[3] - right[3];
  const auto baseline_m = translation_px_m / left[0];
  const auto principal_offset = left[2] - right[2];
  return {
      1.0, 0.0, 0.0, -left[2],
      0.0, 1.0, 0.0, -left[6],
      0.0, 0.0, 0.0, left[0],
      0.0, 0.0, 1.0 / baseline_m, -principal_offset / baseline_m,
  };
}

void validate_projection_pair(const std::array<double, 12>& left,
                              const std::array<double, 12>& right) {
  validate_projection(left, "left projection");
  validate_projection(right, "right projection");
  if (!approximately_equal(left[0], right[0]) ||
      !approximately_equal(left[5], right[5]) ||
      !approximately_equal(left[6], right[6])) {
    throw std::invalid_argument(
        "rectified projections must share focal lengths and vertical principal point");
  }
  if (!approximately_equal(left[3], 0.0)) {
    throw std::invalid_argument(
        "left rectified projection must define the zero-translation reference");
  }
  if (left[3] - right[3] <= 0.0) {
    throw std::invalid_argument(
        "right projection must encode a positive horizontal baseline");
  }
}

[[nodiscard]] std::array<double, 12> resample_projection(
    const std::array<double, 12>& source, const double x_scale,
    const double y_scale, const double x_offset, const double y_offset) {
  auto result = source;
  result[0] = source[0] / x_scale;
  result[2] = (source[2] - x_offset) / x_scale;
  result[3] = source[3] / x_scale;
  result[5] = source[5] / y_scale;
  result[6] = (source[6] - y_offset) / y_scale;
  result[7] = source[7] / y_scale;
  return result;
}

[[nodiscard]] ValidityCode validity_code(const std::uint8_t value) noexcept {
  if (value > static_cast<std::uint8_t>(
                  ValidityCode::kQuantizationOverflow)) {
    return ValidityCode::kUnknown;
  }
  return static_cast<ValidityCode>(value);
}

[[nodiscard]] std::uint8_t validity_byte(const ValidityCode value) noexcept {
  return static_cast<std::uint8_t>(value);
}

}  // namespace

PixelGrid make_reference_grid(const std::uint32_t width,
                              const std::uint32_t height) {
  PixelGrid result{width, height, width, height, 1.0, 1.0, 0.0, 0.0};
  validate(result);
  return result;
}

PixelGrid make_resampled_grid(const PixelGrid& source,
                              const std::uint32_t width,
                              const std::uint32_t height) {
  validate(source);
  if (width == 0U || height == 0U) {
    throw std::invalid_argument("resampled grid dimensions must be positive");
  }
  const auto local_x_scale = static_cast<double>(source.width) /
                             static_cast<double>(width);
  const auto local_y_scale = static_cast<double>(source.height) /
                             static_cast<double>(height);
  const auto local_x_offset = (local_x_scale - 1.0) / 2.0;
  const auto local_y_offset = (local_y_scale - 1.0) / 2.0;
  PixelGrid result;
  result.width = width;
  result.height = height;
  result.reference_width = source.reference_width;
  result.reference_height = source.reference_height;
  result.x_scale_to_reference =
      source.x_scale_to_reference * local_x_scale;
  result.y_scale_to_reference =
      source.y_scale_to_reference * local_y_scale;
  result.x_offset_to_reference = source.x_scale_to_reference * local_x_offset +
                                 source.x_offset_to_reference;
  result.y_offset_to_reference = source.y_scale_to_reference * local_y_offset +
                                 source.y_offset_to_reference;
  validate(result);
  return result;
}

bool same_grid(const PixelGrid& left, const PixelGrid& right) noexcept {
  return left.width == right.width && left.height == right.height &&
         left.reference_width == right.reference_width &&
         left.reference_height == right.reference_height &&
         approximately_equal(left.x_scale_to_reference,
                             right.x_scale_to_reference) &&
         approximately_equal(left.y_scale_to_reference,
                             right.y_scale_to_reference) &&
         approximately_equal(left.x_offset_to_reference,
                             right.x_offset_to_reference) &&
         approximately_equal(left.y_offset_to_reference,
                             right.y_offset_to_reference);
}

void validate(const PixelGrid& grid) {
  if (grid.width == 0U || grid.height == 0U || grid.reference_width == 0U ||
      grid.reference_height == 0U || grid.width > kMaximumGridDimension ||
      grid.height > kMaximumGridDimension ||
      grid.reference_width > kMaximumGridDimension ||
      grid.reference_height > kMaximumGridDimension) {
    throw std::invalid_argument("pixel grid dimensions must be in 1..16384");
  }
  require_finite(grid.x_scale_to_reference, "pixel grid x scale");
  require_finite(grid.y_scale_to_reference, "pixel grid y scale");
  require_finite(grid.x_offset_to_reference, "pixel grid x offset");
  require_finite(grid.y_offset_to_reference, "pixel grid y offset");
  if (grid.x_scale_to_reference <= 0.0 ||
      grid.y_scale_to_reference <= 0.0) {
    throw std::invalid_argument("pixel grid scales must be positive");
  }
  const auto last_x = grid.x_offset_to_reference +
                      static_cast<double>(grid.width - 1U) *
                          grid.x_scale_to_reference;
  const auto last_y = grid.y_offset_to_reference +
                      static_cast<double>(grid.height - 1U) *
                          grid.y_scale_to_reference;
  if (grid.x_offset_to_reference < -0.5 - kComparisonTolerance ||
      grid.y_offset_to_reference < -0.5 - kComparisonTolerance ||
      last_x > static_cast<double>(grid.reference_width) - 0.5 +
                   kComparisonTolerance ||
      last_y > static_cast<double>(grid.reference_height) - 0.5 +
                   kComparisonTolerance) {
    throw std::invalid_argument(
        "pixel grid mapping extends beyond the reference image");
  }
}

CalibrationIdentity calibration_identity_from_sha256(const std::string_view hex) {
  if (hex.size() != 64U) {
    throw std::invalid_argument("calibration SHA-256 must contain 64 hex digits");
  }
  CalibrationIdentity result;
  for (std::size_t index = 0; index < result.sha256.size(); ++index) {
    const auto high = hex_value(hex[index * 2U]);
    const auto low = hex_value(hex[index * 2U + 1U]);
    result.sha256[index] = static_cast<std::uint8_t>((high << 4U) | low);
  }
  return result;
}

std::string calibration_identity_to_sha256(
    const CalibrationIdentity& identity) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64U);
  for (const auto byte : identity.sha256) {
    result.push_back(hex[(byte >> 4U) & 0x0fU]);
    result.push_back(hex[byte & 0x0fU]);
  }
  return result;
}

bool is_present(const CalibrationIdentity& identity) noexcept {
  return std::ranges::any_of(identity.sha256,
                             [](const std::uint8_t byte) { return byte != 0U; });
}

bool metric_depth_authorized(const CalibrationAuthority authority) noexcept {
  return authority == CalibrationAuthority::kUserValidated ||
         authority == CalibrationAuthority::kFactoryValidated;
}

RectifiedCalibrationView make_rectified_calibration(
    const CalibrationIdentity& identity, const CalibrationAuthority authority,
    const PixelGrid& grid,
    const std::array<double, 12>& left_projection,
    const std::array<double, 12>& right_projection) {
  if (!is_present(identity)) {
    throw std::invalid_argument("calibration identity must be content-addressed");
  }
  validate(grid);
  validate_projection_pair(left_projection, right_projection);
  RectifiedCalibrationView result;
  result.identity = identity;
  result.authority = authority;
  result.grid = grid;
  result.left_projection = left_projection;
  result.right_projection = right_projection;
  result.reprojection_q = make_q(left_projection, right_projection);
  result.baseline_m =
      (left_projection[3] - right_projection[3]) / left_projection[0];
  validate(result);
  return result;
}

RectifiedCalibrationView resample_rectified_calibration(
    const RectifiedCalibrationView& source, const PixelGrid& target_grid) {
  validate(source);
  validate(target_grid);
  if (source.grid.reference_width != target_grid.reference_width ||
      source.grid.reference_height != target_grid.reference_height) {
    throw std::invalid_argument(
        "resampled calibration must retain the same reference image");
  }
  const auto x_scale =
      target_grid.x_scale_to_reference / source.grid.x_scale_to_reference;
  const auto y_scale =
      target_grid.y_scale_to_reference / source.grid.y_scale_to_reference;
  const auto x_offset =
      (target_grid.x_offset_to_reference - source.grid.x_offset_to_reference) /
      source.grid.x_scale_to_reference;
  const auto y_offset =
      (target_grid.y_offset_to_reference - source.grid.y_offset_to_reference) /
      source.grid.y_scale_to_reference;
  if (x_scale <= 0.0 || y_scale <= 0.0) {
    throw std::invalid_argument("resampled calibration scale must be positive");
  }
  return make_rectified_calibration(
      source.identity, source.authority, target_grid,
      resample_projection(source.left_projection, x_scale, y_scale, x_offset,
                          y_offset),
      resample_projection(source.right_projection, x_scale, y_scale, x_offset,
                          y_offset));
}

void validate(const RectifiedCalibrationView& calibration) {
  if (!is_present(calibration.identity)) {
    throw std::invalid_argument("calibration identity must not be empty");
  }
  switch (calibration.authority) {
    case CalibrationAuthority::kProvisionalRecalibrationRequired:
    case CalibrationAuthority::kUserValidated:
    case CalibrationAuthority::kFactoryValidated:
      break;
    default:
      throw std::invalid_argument("calibration authority is invalid");
  }
  validate(calibration.grid);
  validate_projection_pair(calibration.left_projection,
                           calibration.right_projection);
  require_finite(calibration.baseline_m, "calibration baseline");
  const auto expected_baseline =
      (calibration.left_projection[3] - calibration.right_projection[3]) /
      calibration.left_projection[0];
  if (calibration.baseline_m <= 0.0 ||
      !approximately_equal(calibration.baseline_m, expected_baseline)) {
    throw std::invalid_argument(
        "calibration baseline disagrees with the rectified projections");
  }
  const auto expected_q =
      make_q(calibration.left_projection, calibration.right_projection);
  for (std::size_t index = 0; index < expected_q.size(); ++index) {
    require_finite(calibration.reprojection_q[index], "reprojection Q");
    if (!approximately_equal(calibration.reprojection_q[index],
                             expected_q[index])) {
      throw std::invalid_argument(
          "reprojection Q disagrees with the rectified projections");
    }
  }
}

void validate(const FrameSetIdentity& identity) {
  const auto uuid_present = [](const std::array<std::uint8_t, 16>& uuid) {
    return std::ranges::any_of(uuid,
                               [](const std::uint8_t byte) { return byte != 0U; });
  };
  if (!uuid_present(identity.device_uuid) || !uuid_present(identity.run_uuid) ||
      identity.capture_epoch == 0U || identity.capture_sequence == 0U ||
      identity.configuration_epoch == 0U || identity.capture_time_ns <= 0 ||
      identity.capture_clock != CaptureClock::kBoottime) {
    throw std::invalid_argument(
        "frame identity requires nonzero epochs/sequence and a BOOTTIME timestamp");
  }
}

void validate(const RegionU32& region, const PixelGrid& grid) {
  validate(grid);
  if (region.width == 0U || region.height == 0U || region.x > grid.width ||
      region.y > grid.height || region.width > grid.width - region.x ||
      region.height > grid.height - region.y) {
    throw std::invalid_argument("valid ROI must be nonempty and inside its grid");
  }
}

void validate(const RectifiedImageMetadata& metadata) {
  validate(metadata.grid);
  validate(metadata.valid_roi, metadata.grid);
  switch (metadata.camera_role) {
    case CameraRole::kLogicalLeft:
    case CameraRole::kLogicalRight:
      break;
    default:
      throw std::invalid_argument("rectified image camera role is invalid");
  }
  switch (metadata.format) {
    case ImageProductFormat::kGray8:
    case ImageProductFormat::kBgr8:
    case ImageProductFormat::kRgb8:
      break;
    default:
      throw std::invalid_argument("rectified image format is invalid");
  }
}

void validate(const DisparityMetadata& metadata) {
  validate(metadata.grid);
  validate(metadata.valid_roi, metadata.grid);
  if (!std::isfinite(metadata.accepted_min_disparity_px) ||
      !std::isfinite(metadata.accepted_max_disparity_px) ||
      metadata.accepted_max_disparity_px <=
          metadata.accepted_min_disparity_px ||
      !std::isfinite(metadata.algorithmic_quantization_px) ||
      metadata.algorithmic_quantization_px <= 0.0F) {
    throw std::invalid_argument(
        "disparity interval and algorithmic quantization must be finite and ordered");
  }
  if (!metadata.invalid_is_nan) {
    throw std::invalid_argument("float disparity must encode invalid values as NaN");
  }
  if (metadata.confidence_model == ConfidenceModel::kNone) {
    throw std::invalid_argument(
        "disparity product must name its confidence model");
  }
}

void validate(const DepthMetadata& metadata) {
  validate(metadata.grid);
  if (!std::isfinite(metadata.minimum_depth_m) ||
      !std::isfinite(metadata.maximum_depth_m) ||
      metadata.minimum_depth_m <= 0.0F ||
      metadata.maximum_depth_m <= metadata.minimum_depth_m) {
    throw std::invalid_argument(
        "depth interval must be finite, positive, and strictly ordered");
  }
  if (!metadata.invalid_is_nan ||
      metadata.convention != DepthConvention::kOpticalAxisZ ||
      metadata.unit != DepthUnit::kMetres) {
    throw std::invalid_argument(
        "canonical depth must be optical-axis Z in metres with NaN invalids");
  }
}

void validate(const ProductSetMetadata& metadata) {
  if (metadata.schema_version != kProductSchemaVersion) {
    throw std::invalid_argument("unsupported product schema version");
  }
  validate(metadata.frame);
  if (!is_present(metadata.calibration)) {
    throw std::invalid_argument("product set requires a calibration identity");
  }
  switch (metadata.calibration_authority) {
    case CalibrationAuthority::kProvisionalRecalibrationRequired:
    case CalibrationAuthority::kUserValidated:
    case CalibrationAuthority::kFactoryValidated:
      break;
    default:
      throw std::invalid_argument("product calibration authority is invalid");
  }
  validate(metadata.left_rectified);
  validate(metadata.right_rectified);
  if (metadata.left_rectified.camera_role != CameraRole::kLogicalLeft ||
      metadata.right_rectified.camera_role != CameraRole::kLogicalRight ||
      !same_grid(metadata.left_rectified.grid,
                 metadata.right_rectified.grid)) {
    throw std::invalid_argument(
        "product set requires associated logical-left/right rectified images");
  }
  validate(metadata.disparity);
  validate(metadata.depth);
  if (!same_grid(metadata.disparity.grid, metadata.depth.grid)) {
    throw std::invalid_argument(
        "canonical disparity and depth products must share the matching grid");
  }
  if (metadata.disparity.grid.reference_width !=
          metadata.left_rectified.grid.reference_width ||
      metadata.disparity.grid.reference_height !=
          metadata.left_rectified.grid.reference_height) {
    throw std::invalid_argument(
        "matching products must reference the rectified image grid");
  }
}

void validate(const ProductSetMetadata& metadata,
              const RectifiedCalibrationView& image_calibration,
              const RectifiedCalibrationView& matching_calibration) {
  validate(metadata);
  validate(image_calibration);
  validate(matching_calibration);
  if (metadata.calibration.sha256 != image_calibration.identity.sha256 ||
      metadata.calibration.sha256 != matching_calibration.identity.sha256 ||
      metadata.calibration_authority != image_calibration.authority ||
      metadata.calibration_authority != matching_calibration.authority) {
    throw std::invalid_argument(
        "product metadata does not identify its rectified calibration");
  }
  if (!same_grid(metadata.left_rectified.grid, image_calibration.grid) ||
      !same_grid(metadata.right_rectified.grid, image_calibration.grid) ||
      !same_grid(metadata.disparity.grid, matching_calibration.grid) ||
      !same_grid(metadata.depth.grid, matching_calibration.grid)) {
    throw std::invalid_argument(
        "product grids do not match their rectified calibration");
  }
}

std::size_t required_bytes(const PixelGrid& grid,
                           const std::size_t stride_elements,
                           const std::size_t element_size) {
  validate(grid);
  if (stride_elements < grid.width || element_size == 0U) {
    throw std::invalid_argument("plane stride or element size is invalid");
  }
  const auto maximum = std::numeric_limits<std::size_t>::max();
  const auto preceding_rows = static_cast<std::size_t>(grid.height - 1U);
  if (preceding_rows != 0U && stride_elements > maximum / preceding_rows) {
    throw std::overflow_error("plane row addressing overflows size_t");
  }
  const auto final_elements = preceding_rows * stride_elements;
  if (static_cast<std::size_t>(grid.width) > maximum - final_elements) {
    throw std::overflow_error("plane element count overflows size_t");
  }
  const auto element_count = final_elements + grid.width;
  if (element_count > maximum / element_size) {
    throw std::overflow_error("plane byte count overflows size_t");
  }
  return element_count * element_size;
}

void validate(const FloatPlaneView& plane) {
  validate_plane(plane, sizeof(float), "float plane");
}

void validate(const MutableFloatPlaneView& plane) {
  validate_plane(plane, sizeof(float), "mutable float plane");
}

void validate(const BytePlaneView& plane) {
  validate_plane(plane, sizeof(std::uint8_t), "byte plane");
}

void validate(const MutableBytePlaneView& plane) {
  validate_plane(plane, sizeof(std::uint8_t), "mutable byte plane");
}

void validate(const MutableU16PlaneView& plane) {
  validate_plane(plane, sizeof(std::uint16_t), "mutable uint16 plane");
}

void validate(const ImagePlaneView& plane) {
  if (plane.data == nullptr) {
    throw std::invalid_argument("image plane data must not be null");
  }
  validate(plane.grid);
  std::size_t channels{};
  switch (plane.format) {
    case ImageProductFormat::kGray8:
      channels = 1U;
      break;
    case ImageProductFormat::kBgr8:
    case ImageProductFormat::kRgb8:
      channels = 3U;
      break;
    default:
      throw std::invalid_argument("image plane format is invalid");
  }
  const auto row_bytes = static_cast<std::size_t>(plane.grid.width) * channels;
  if (plane.stride_bytes < row_bytes) {
    throw std::invalid_argument("image plane stride does not cover one row");
  }
  const auto maximum = std::numeric_limits<std::size_t>::max();
  const auto preceding_rows = static_cast<std::size_t>(plane.grid.height - 1U);
  if (preceding_rows != 0U &&
      plane.stride_bytes > maximum / preceding_rows) {
    throw std::overflow_error("image plane row addressing overflows size_t");
  }
  const auto final_offset = preceding_rows * plane.stride_bytes;
  if (row_bytes > maximum - final_offset) {
    throw std::overflow_error("image plane byte count overflows size_t");
  }
}

void validate(const MutableDisparityProductsView& products) {
  validate(products.disparity);
  validate(products.confidence);
  validate(products.validity);
  if (!same_grid(products.disparity.grid, products.confidence.grid) ||
      !same_grid(products.disparity.grid, products.validity.grid)) {
    throw std::invalid_argument(
        "disparity, confidence, and validity planes must share one grid");
  }
}

void validate(const ProductSetView& products) {
  validate(products.metadata);
  validate(products.left_rectified);
  validate(products.right_rectified);
  validate(products.disparity);
  validate(products.depth_m);
  validate(products.confidence);
  validate(products.validity);
  if (!same_grid(products.left_rectified.grid,
                 products.metadata.left_rectified.grid) ||
      !same_grid(products.right_rectified.grid,
                 products.metadata.right_rectified.grid) ||
      products.left_rectified.format !=
          products.metadata.left_rectified.format ||
      products.right_rectified.format !=
          products.metadata.right_rectified.format ||
      !same_grid(products.disparity.grid, products.metadata.disparity.grid) ||
      !same_grid(products.depth_m.grid, products.metadata.depth.grid) ||
      !same_grid(products.confidence.grid, products.metadata.disparity.grid) ||
      !same_grid(products.validity.grid, products.metadata.disparity.grid)) {
    throw std::invalid_argument(
        "product plane layout disagrees with the associated metadata");
  }
}

DisparityMetadata make_resampled_disparity_metadata(
    const DisparityMetadata& source, const PixelGrid& target_grid) {
  validate(source);
  validate(target_grid);
  if (source.grid.reference_width != target_grid.reference_width ||
      source.grid.reference_height != target_grid.reference_height) {
    throw std::invalid_argument(
        "resampled disparity metadata must retain the reference image");
  }
  const auto disparity_scale = source.grid.x_scale_to_reference /
                               target_grid.x_scale_to_reference;
  if (!std::isfinite(disparity_scale) || disparity_scale <= 0.0) {
    throw std::invalid_argument("derived disparity scale must be positive");
  }

  const auto supported_interval = [](const std::uint32_t target_extent,
                                     const double target_scale,
                                     const double target_offset,
                                     const std::uint32_t source_extent,
                                     const double source_scale,
                                     const double source_offset,
                                     const std::uint32_t roi_start,
                                     const std::uint32_t roi_extent) {
    std::uint32_t first = target_extent;
    std::uint32_t last = 0U;
    bool found = false;
    const auto roi_end = roi_start + roi_extent;
    for (std::uint32_t target = 0U; target < target_extent; ++target) {
      const auto reference = static_cast<double>(target) * target_scale +
                             target_offset;
      const auto source_coordinate =
          std::clamp((reference - source_offset) / source_scale, 0.0,
                     static_cast<double>(source_extent - 1U));
      const auto lower =
          static_cast<std::uint32_t>(std::floor(source_coordinate));
      const auto upper =
          static_cast<std::uint32_t>(std::ceil(source_coordinate));
      if (lower >= roi_start && upper < roi_end) {
        if (!found) {
          first = target;
          found = true;
        }
        last = target + 1U;
      }
    }
    if (!found) {
      throw std::invalid_argument(
          "source valid ROI has no support on the target grid");
    }
    return std::array<std::uint32_t, 2>{first, last - first};
  };

  const auto horizontal = supported_interval(
      target_grid.width, target_grid.x_scale_to_reference,
      target_grid.x_offset_to_reference, source.grid.width,
      source.grid.x_scale_to_reference, source.grid.x_offset_to_reference,
      source.valid_roi.x, source.valid_roi.width);
  const auto vertical = supported_interval(
      target_grid.height, target_grid.y_scale_to_reference,
      target_grid.y_offset_to_reference, source.grid.height,
      source.grid.y_scale_to_reference, source.grid.y_offset_to_reference,
      source.valid_roi.y, source.valid_roi.height);

  DisparityMetadata result = source;
  result.grid = target_grid;
  result.valid_roi = {horizontal[0], vertical[0], horizontal[1], vertical[1]};
  result.accepted_min_disparity_px = static_cast<float>(
      static_cast<double>(source.accepted_min_disparity_px) * disparity_scale);
  result.accepted_max_disparity_px = static_cast<float>(
      static_cast<double>(source.accepted_max_disparity_px) * disparity_scale);
  result.algorithmic_quantization_px = static_cast<float>(
      static_cast<double>(source.algorithmic_quantization_px) *
      disparity_scale);
  result.derivation =
      DisparityDerivation::kInvalidAwareBilinearCompatibility;
  validate(result);
  return result;
}

void resample_disparity_invalid_aware(
    const FloatPlaneView& source_disparity,
    const BytePlaneView& source_confidence,
    const BytePlaneView& source_validity,
    const MutableDisparityProductsView& target) {
  validate(source_disparity);
  validate(source_confidence);
  validate(source_validity);
  validate(target);
  if (!same_grid(source_disparity.grid, source_confidence.grid) ||
      !same_grid(source_disparity.grid, source_validity.grid)) {
    throw std::invalid_argument("source disparity products must share one grid");
  }
  if (source_disparity.grid.reference_width !=
          target.disparity.grid.reference_width ||
      source_disparity.grid.reference_height !=
          target.disparity.grid.reference_height) {
    throw std::invalid_argument(
        "disparity resampling must retain the reference image");
  }
  const auto disparity_scale = source_disparity.grid.x_scale_to_reference /
                               target.disparity.grid.x_scale_to_reference;
  if (!std::isfinite(disparity_scale) || disparity_scale <= 0.0) {
    throw std::invalid_argument("disparity coordinate scale must be positive");
  }

  constexpr double weight_epsilon = 1e-12;
  const auto source_width = source_disparity.grid.width;
  const auto source_height = source_disparity.grid.height;
  for (std::uint32_t target_y = 0; target_y < target.disparity.grid.height;
       ++target_y) {
    const auto reference_y =
        static_cast<double>(target_y) *
            target.disparity.grid.y_scale_to_reference +
        target.disparity.grid.y_offset_to_reference;
    const auto source_y =
        (reference_y - source_disparity.grid.y_offset_to_reference) /
        source_disparity.grid.y_scale_to_reference;
    const auto clamped_y = std::clamp(source_y, 0.0,
                                      static_cast<double>(source_height - 1U));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clamped_y));
    const auto y1 = std::min(y0 + 1U, source_height - 1U);
    const auto wy = clamped_y - static_cast<double>(y0);

    for (std::uint32_t target_x = 0; target_x < target.disparity.grid.width;
         ++target_x) {
      const auto reference_x =
          static_cast<double>(target_x) *
              target.disparity.grid.x_scale_to_reference +
          target.disparity.grid.x_offset_to_reference;
      const auto source_x =
          (reference_x - source_disparity.grid.x_offset_to_reference) /
          source_disparity.grid.x_scale_to_reference;
      const auto clamped_x = std::clamp(
          source_x, 0.0, static_cast<double>(source_width - 1U));
      const auto x0 = static_cast<std::uint32_t>(std::floor(clamped_x));
      const auto x1 = std::min(x0 + 1U, source_width - 1U);
      const auto wx = clamped_x - static_cast<double>(x0);

      const std::array<std::uint32_t, 4> xs{x0, x1, x0, x1};
      const std::array<std::uint32_t, 4> ys{y0, y0, y1, y1};
      const std::array<double, 4> weights{
          (1.0 - wx) * (1.0 - wy), wx * (1.0 - wy),
          (1.0 - wx) * wy, wx * wy};

      bool valid = true;
      auto invalid_code = ValidityCode::kUnknown;
      double strongest_invalid_weight = -1.0;
      double disparity_sum = 0.0;
      auto confidence = std::numeric_limits<std::uint8_t>::max();
      for (std::size_t sample = 0; sample < weights.size(); ++sample) {
        const auto weight = weights[sample];
        if (weight <= weight_epsilon) {
          continue;
        }
        const auto source_index =
            static_cast<std::size_t>(ys[sample]) *
                source_disparity.stride_elements +
            xs[sample];
        const auto code = validity_code(source_validity.data[
            static_cast<std::size_t>(ys[sample]) *
                source_validity.stride_elements +
            xs[sample]]);
        const auto value = source_disparity.data[source_index];
        const auto source_confidence_index =
            static_cast<std::size_t>(ys[sample]) *
                source_confidence.stride_elements +
            xs[sample];
        const auto sample_confidence =
            source_confidence.data[source_confidence_index];
        if (code != ValidityCode::kValid || !std::isfinite(value) ||
            sample_confidence == 0U) {
          valid = false;
          if (weight > strongest_invalid_weight) {
            strongest_invalid_weight = weight;
            if (code != ValidityCode::kValid) {
              invalid_code = code;
            } else if (!std::isfinite(value)) {
              invalid_code = ValidityCode::kNonFinite;
            } else {
              invalid_code = ValidityCode::kInconsistent;
            }
          }
          continue;
        }
        disparity_sum += weight * static_cast<double>(value);
        confidence = std::min(confidence, sample_confidence);
      }

      const auto target_disparity_index =
          static_cast<std::size_t>(target_y) *
              target.disparity.stride_elements +
          target_x;
      const auto target_confidence_index =
          static_cast<std::size_t>(target_y) *
              target.confidence.stride_elements +
          target_x;
      const auto target_validity_index =
          static_cast<std::size_t>(target_y) * target.validity.stride_elements +
          target_x;
      const auto scaled = disparity_sum * disparity_scale;
      if (!valid || !std::isfinite(scaled)) {
        target.disparity.data[target_disparity_index] =
            std::numeric_limits<float>::quiet_NaN();
        target.confidence.data[target_confidence_index] = 0U;
        target.validity.data[target_validity_index] = validity_byte(
            valid ? ValidityCode::kNonFinite : invalid_code);
      } else {
        target.disparity.data[target_disparity_index] =
            static_cast<float>(scaled);
        target.confidence.data[target_confidence_index] = confidence;
        target.validity.data[target_validity_index] =
            validity_byte(ValidityCode::kValid);
      }
    }
  }
}

}  // namespace depthd
