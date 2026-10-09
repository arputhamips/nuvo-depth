#include "depthd/calibration_store.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "depthd/config.hpp"

namespace {

std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value;
  if (!output) throw std::runtime_error("failed to write test bundle");
}

}  // namespace

int main() {
  const auto root = std::filesystem::path(DEPTHD_SOURCE_DIR) /
                    "config/calibration/nuvo_depth_100mm";
  const auto prepared = depthd::load_and_prepare_stereo_calibration(
      root / "front_left.yaml", root / "front_right.yaml", 364U, 272U);
  assert(prepared.authority ==
         depthd::CalibrationAuthority::kProvisionalRecalibrationRequired);
  assert(prepared.image_view.grid.width == 728U);
  assert(prepared.matching_view.grid.width == 364U);
  assert(std::abs(prepared.image_view.baseline_m - 0.1) < 1e-8);
  assert(depthd::is_valid(prepared.maps));
  assert(!prepared.left.has_rectified_output_correction);
  assert(prepared.right.has_rectified_output_correction);
  assert(std::abs(prepared.right.rectified_output_correction[3] +
                  0.0091791619) < 1e-12);
  assert(prepared.maps.left.source_x.size() == 728U * 544U);
  assert(depthd::calibration_identity_to_sha256(prepared.identity).size() ==
         64U);

  bool rejected = false;
  try {
    (void)depthd::load_and_prepare_stereo_calibration(
        root / "front_left.yaml", root / "front_left.yaml", 364U, 272U);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  if (!rejected) {
    throw std::runtime_error("same camera calibration pair was accepted");
  }

  const auto temporary = std::filesystem::temp_directory_path() /
                         ("nuvo-calibration-store-" + std::to_string(
                             std::chrono::steady_clock::now()
                                 .time_since_epoch().count()));
  const std::string bundle_id(64U, 'a');
  const auto bundle = temporary / bundle_id;
  std::filesystem::create_directories(bundle);
  auto left = read(root / "front_left.yaml");
  auto right = read(root / "front_right.yaml");
  const std::string provisional = "provisional_recalibration_required";
  left.replace(left.find(provisional), provisional.size(), "user_validated");
  right.replace(right.find(provisional), provisional.size(), "user_validated");
  write(bundle / "front_left.yaml", left);
  write(bundle / "front_right.yaml", right);
  write(bundle / "validation.json", "{\"status\":\"pass\"}\n");
  const auto hash = [&](const char* name) {
    return depthd::config::sha256_hex(read(bundle / name));
  };
  write(bundle / "manifest.json",
        "{\"schema_version\":1,\"bundle_id\":\"" + bundle_id +
            "\",\"unit_id\":\"unit-1\",\"left_camera_id\":\"left-id\","
            "\"right_camera_id\":\"right-id\",\"preset_id\":\"balanced_728x544_10\","
            "\"authority\":\"user_validated\",\"assets\":{"
            "\"front_left.yaml\":\"" + hash("front_left.yaml") +
            "\",\"front_right.yaml\":\"" + hash("front_right.yaml") +
            "\",\"validation.json\":\"" + hash("validation.json") +
            "\"}}\n");
  const auto active = depthd::load_and_prepare_active_calibration(
      bundle, depthd::CalibrationAuthority::kUserValidated, 364U, 272U);
  assert(active.bundle_id == bundle_id && active.unit_id == "unit-1" &&
         active.left_camera_id == "left-id" &&
         active.preset_id == "balanced_728x544_10");
  assert(depthd::calibration_identity_to_sha256(active.identity) == bundle_id);
  write(bundle / "front_left.yaml", left + "# tamper\n");
  rejected = false;
  try {
    (void)depthd::load_and_prepare_active_calibration(
        bundle, depthd::CalibrationAuthority::kUserValidated, 364U, 272U);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  std::filesystem::remove_all(temporary);
  if (!rejected) {
    throw std::runtime_error("tampered activated bundle was accepted");
  }
}
