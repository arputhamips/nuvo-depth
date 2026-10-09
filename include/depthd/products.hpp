#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "depthd/calibration_view.hpp"
#include "depthd/output_selection.hpp"

namespace depthd {

inline constexpr std::uint32_t kProductSchemaVersion = 1;

enum class CaptureClock : std::uint8_t {
  kUnknown = 0,
  kBoottime = 1,
};

struct FrameSetIdentity {
  std::array<std::uint8_t, 16> device_uuid{};
  std::array<std::uint8_t, 16> run_uuid{};
  std::uint64_t capture_epoch{};
  std::uint64_t capture_sequence{};
  std::uint64_t configuration_epoch{};
  std::int64_t capture_time_ns{};
  CaptureClock capture_clock{CaptureClock::kUnknown};
};

struct RegionU32 {
  std::uint32_t x{};
  std::uint32_t y{};
  std::uint32_t width{};
  std::uint32_t height{};
};

enum class CameraRole : std::uint8_t {
  kLogicalLeft = 1,
  kLogicalRight = 2,
};

enum class ImageProductFormat : std::uint8_t {
  kGray8 = 1,
  kBgr8 = 2,
  kRgb8 = 3,
};

struct RectifiedImageMetadata {
  PixelGrid grid{};
  RegionU32 valid_roi{};
  CameraRole camera_role{CameraRole::kLogicalLeft};
  ImageProductFormat format{ImageProductFormat::kGray8};
};

enum class ValidityCode : std::uint8_t {
  kUnknown = 0,
  kValid = 1,
  kOutsideRoi = 2,
  kOutsideDisparityRange = 3,
  kSaturated = 4,
  kLowTexture = 5,
  kInconsistent = 6,
  kOccluded = 7,
  kNonFinite = 8,
  kReprojectionFailure = 9,
  kOutsideDepthRange = 10,
  kQuantizationOverflow = 11,
};

enum class ConfidenceModel : std::uint8_t {
  kNone = 0,
  // Binary matcher support: 0 is invalid and 255 is accepted. This is not a
  // probability or a calibrated confidence estimate.
  kBinarySgbmSupportV1 = 1,
  kBinaryBmSupportV1 = 2,
};

enum class DisparityDerivation : std::uint8_t {
  kNativeMatcherGrid = 0,
  kInvalidAwareBilinearCompatibility = 1,
};

struct DisparityMetadata {
  PixelGrid grid{};
  RegionU32 valid_roi{};
  float accepted_min_disparity_px{};
  float accepted_max_disparity_px{};  // Exclusive.
  float algorithmic_quantization_px{};
  bool invalid_is_nan{true};
  ConfidenceModel confidence_model{ConfidenceModel::kNone};
  DisparityDerivation derivation{DisparityDerivation::kNativeMatcherGrid};
};

enum class DepthConvention : std::uint8_t {
  kOpticalAxisZ = 1,
};

enum class DepthUnit : std::uint8_t {
  kMetres = 1,
};

struct DepthMetadata {
  PixelGrid grid{};
  float minimum_depth_m{};
  float maximum_depth_m{};
  bool invalid_is_nan{true};
  DepthConvention convention{DepthConvention::kOpticalAxisZ};
  DepthUnit unit{DepthUnit::kMetres};
};

struct ProductSetMetadata {
  std::uint32_t schema_version{kProductSchemaVersion};
  FrameSetIdentity frame{};
  CalibrationIdentity calibration{};
  CalibrationAuthority calibration_authority{
      CalibrationAuthority::kProvisionalRecalibrationRequired};
  RectifiedImageMetadata left_rectified{};
  RectifiedImageMetadata right_rectified{};
  DisparityMetadata disparity{};
  DepthMetadata depth{};
};

struct FloatPlaneView {
  const float* data{};
  PixelGrid grid{};
  std::size_t stride_elements{};
};

struct MutableFloatPlaneView {
  float* data{};
  PixelGrid grid{};
  std::size_t stride_elements{};
};

struct BytePlaneView {
  const std::uint8_t* data{};
  PixelGrid grid{};
  std::size_t stride_elements{};
};

struct MutableBytePlaneView {
  std::uint8_t* data{};
  PixelGrid grid{};
  std::size_t stride_elements{};
};

struct MutableU16PlaneView {
  std::uint16_t* data{};
  PixelGrid grid{};
  std::size_t stride_elements{};
};

struct ImagePlaneView {
  const std::uint8_t* data{};
  PixelGrid grid{};
  std::size_t stride_bytes{};
  ImageProductFormat format{ImageProductFormat::kGray8};
};

struct MutableDisparityProductsView {
  MutableFloatPlaneView disparity{};
  MutableBytePlaneView confidence{};
  MutableBytePlaneView validity{};
};

struct ProductSetView {
  ProductSetMetadata metadata{};
  ImagePlaneView left_rectified{};
  ImagePlaneView right_rectified{};
  FloatPlaneView disparity{};
  FloatPlaneView depth_m{};
  BytePlaneView confidence{};
  BytePlaneView validity{};
  ipc_v2::ProductMask available_products{ipc_v2::kAllProducts};
};

void validate(const FrameSetIdentity& identity);
void validate(const RegionU32& region, const PixelGrid& grid);
void validate(const RectifiedImageMetadata& metadata);
void validate(const DisparityMetadata& metadata);
void validate(const DepthMetadata& metadata);
void validate(const ProductSetMetadata& metadata);
void validate(const ProductSetMetadata& metadata,
              const RectifiedCalibrationView& image_calibration,
              const RectifiedCalibrationView& matching_calibration);
void validate(const FloatPlaneView& plane);
void validate(const MutableFloatPlaneView& plane);
void validate(const BytePlaneView& plane);
void validate(const MutableBytePlaneView& plane);
void validate(const MutableU16PlaneView& plane);
void validate(const ImagePlaneView& plane);
void validate(const MutableDisparityProductsView& products);
void validate(const ProductSetView& products);

[[nodiscard]] std::size_t required_bytes(const PixelGrid& grid,
                                         std::size_t stride_elements,
                                         std::size_t element_size);

[[nodiscard]] DisparityMetadata make_resampled_disparity_metadata(
    const DisparityMetadata& source, const PixelGrid& target_grid);

// A compatibility upsampler. A target sample is valid only when every source
// sample with non-zero bilinear weight is valid. Invalid samples therefore
// cannot contaminate retained disparity values.
void resample_disparity_invalid_aware(
    const FloatPlaneView& source_disparity,
    const BytePlaneView& source_confidence,
    const BytePlaneView& source_validity,
    const MutableDisparityProductsView& target);

struct DepthConversionOptions {
  float minimum_depth_m{0.001F};
  float maximum_depth_m{1000.0F};
  bool allow_provisional_for_engineering{false};
};

struct DepthConversionStats {
  std::uint64_t pixel_count{};
  std::uint64_t valid_count{};
  std::uint64_t invalid_input_count{};
  std::uint64_t nonfinite_count{};
  std::uint64_t reprojection_failure_count{};
  std::uint64_t outside_depth_range_count{};
};

// Produces optical-axis Z in metres. Input and output validity are separate so
// consumers can retain disparity classification while applying depth limits.
[[nodiscard]] DepthConversionStats convert_disparity_to_depth(
    const FloatPlaneView& disparity, const BytePlaneView& disparity_validity,
    const RectifiedCalibrationView& calibration,
    const DepthConversionOptions& options, const MutableFloatPlaneView& depth_m,
    const MutableBytePlaneView& depth_validity);

struct DepthQuantizationStats {
  std::uint64_t valid_count{};
  std::uint64_t invalid_input_count{};
  std::uint64_t nonfinite_count{};
  std::uint64_t overflow_count{};
};

inline constexpr std::uint16_t kInvalidDepthMillimetres = 0;

// Converts metres to integer millimetres with round-to-nearest. Zero is
// reserved for invalid data; overflow is classified instead of saturated.
[[nodiscard]] DepthQuantizationStats quantize_depth_to_u16_mm(
    const FloatPlaneView& depth_m, const BytePlaneView& depth_validity,
    const MutableU16PlaneView& depth_mm,
    const MutableBytePlaneView& quantized_validity);

}  // namespace depthd
