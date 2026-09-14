#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "depthd/calibration_view.hpp"
#include "depthd/rectification_backend.hpp"

namespace depthd {

struct CameraCalibrationDocument {
  std::string camera_name;
  std::string calibration_status;
  std::uint32_t image_width{};
  std::uint32_t image_height{};
  double baseline_m{};
  std::array<double, 9> camera_matrix{};
  std::vector<double> distortion_coefficients;
  std::array<double, 9> rectification_matrix{};
  std::array<double, 12> projection_matrix{};
};

// Prepared once at configuration activation. The content identity covers both
// source documents in their exact byte representation and is carried by every
// derived product.
struct PreparedStereoCalibration {
  std::string bundle_id;
  std::string unit_id;
  std::string left_camera_id;
  std::string right_camera_id;
  std::string preset_id;
  CameraCalibrationDocument left;
  CameraCalibrationDocument right;
  CalibrationIdentity identity{};
  CalibrationAuthority authority{
      CalibrationAuthority::kProvisionalRecalibrationRequired};
  RectifiedCalibrationView image_view{};
  RectifiedCalibrationView matching_view{};
  StereoRectificationMaps maps{};
};

[[nodiscard]] PreparedStereoCalibration load_and_prepare_stereo_calibration(
    const std::filesystem::path& left_path,
    const std::filesystem::path& right_path, std::uint32_t matching_width,
    std::uint32_t matching_height);

// Loads an activated content-addressed bundle, verifies its asset hashes and
// passing validation record, enforces its namespace authority, and uses the
// full bundle identity in every derived frame product.
[[nodiscard]] PreparedStereoCalibration load_and_prepare_active_calibration(
    const std::filesystem::path& bundle_path,
    CalibrationAuthority expected_authority, std::uint32_t matching_width,
    std::uint32_t matching_height);

}  // namespace depthd
