#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace depthd {

// Pixel coordinates are mapped to the reference image as:
//   reference = pixel * scale_to_reference + offset_to_reference
// This makes reduced matching grids explicit instead of presenting an
// interpolated image-sized result as a native measurement.
struct PixelGrid {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t reference_width{};
  std::uint32_t reference_height{};
  double x_scale_to_reference{1.0};
  double y_scale_to_reference{1.0};
  double x_offset_to_reference{};
  double y_offset_to_reference{};
};

struct CalibrationIdentity {
  std::array<std::uint8_t, 32> sha256{};
};

enum class CalibrationAuthority : std::uint8_t {
  kProvisionalRecalibrationRequired = 0,
  kUserValidated = 1,
  kFactoryValidated = 2,
};

// A small, owning value type suitable for attaching to a frame generation.
// Projection matrices and Q are row-major and apply to grid pixel units.
struct RectifiedCalibrationView {
  CalibrationIdentity identity{};
  CalibrationAuthority authority{
      CalibrationAuthority::kProvisionalRecalibrationRequired};
  PixelGrid grid{};
  std::array<double, 12> left_projection{};
  std::array<double, 12> right_projection{};
  std::array<double, 16> reprojection_q{};
  double baseline_m{};
};

[[nodiscard]] PixelGrid make_reference_grid(std::uint32_t width,
                                            std::uint32_t height);
[[nodiscard]] PixelGrid make_resampled_grid(const PixelGrid& source,
                                            std::uint32_t width,
                                            std::uint32_t height);
[[nodiscard]] bool same_grid(const PixelGrid& left,
                             const PixelGrid& right) noexcept;
void validate(const PixelGrid& grid);

[[nodiscard]] CalibrationIdentity calibration_identity_from_sha256(
    std::string_view hex);
[[nodiscard]] std::string calibration_identity_to_sha256(
    const CalibrationIdentity& identity);
[[nodiscard]] bool is_present(const CalibrationIdentity& identity) noexcept;
[[nodiscard]] bool metric_depth_authorized(
    CalibrationAuthority authority) noexcept;

[[nodiscard]] RectifiedCalibrationView make_rectified_calibration(
    const CalibrationIdentity& identity, CalibrationAuthority authority,
    const PixelGrid& grid, const std::array<double, 12>& left_projection,
    const std::array<double, 12>& right_projection);
[[nodiscard]] RectifiedCalibrationView resample_rectified_calibration(
    const RectifiedCalibrationView& source, const PixelGrid& target_grid);
void validate(const RectifiedCalibrationView& calibration);

}  // namespace depthd
