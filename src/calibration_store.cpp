#include "depthd/calibration_store.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <yaml-cpp/yaml.h>

#include "depthd/config.hpp"

namespace depthd {
namespace {

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open calibration document: " +
                             path.string());
  }
  std::ostringstream bytes;
  bytes << input.rdbuf();
  if (!input.good() && !input.eof()) {
    throw std::runtime_error("cannot read calibration document: " +
                             path.string());
  }
  return bytes.str();
}

void mapping(const YAML::Node& node, const std::filesystem::path& path,
             const std::set<std::string>& required,
             const std::set<std::string>& optional = {}) {
  if (!node.IsMap()) {
    throw std::invalid_argument(path.string() + ": root must be a mapping");
  }
  std::set<std::string> seen;
  for (const auto& entry : node) {
    const auto key = entry.first.as<std::string>();
    if (!required.contains(key) && !optional.contains(key)) {
      throw std::invalid_argument(path.string() + ": unknown key '" + key +
                                  "'");
    }
    if (!seen.insert(key).second) {
      throw std::invalid_argument(path.string() + ": duplicate key '" + key +
                                  "'");
    }
  }
  for (const auto& key : required) {
    if (!node[key]) {
      throw std::invalid_argument(path.string() + ": missing key '" + key +
                                  "'");
    }
  }
}

template <std::size_t Count>
[[nodiscard]] std::array<double, Count> fixed_matrix(
    const YAML::Node& node, const std::filesystem::path& path,
    const std::string_view name, const std::uint32_t rows,
    const std::uint32_t cols) {
  if (!node.IsMap() || !node["rows"] || !node["cols"] || !node["data"] ||
      node.size() != 3U || node["rows"].as<std::uint32_t>() != rows ||
      node["cols"].as<std::uint32_t>() != cols ||
      !node["data"].IsSequence() || node["data"].size() != Count) {
    throw std::invalid_argument(path.string() + ": " + std::string(name) +
                                " has an invalid matrix shape");
  }
  std::array<double, Count> result{};
  for (std::size_t index = 0; index < Count; ++index) {
    result[index] = node["data"][index].as<double>();
    if (!std::isfinite(result[index])) {
      throw std::invalid_argument(path.string() + ": " + std::string(name) +
                                  " contains a non-finite value");
    }
  }
  return result;
}

[[nodiscard]] CameraCalibrationDocument parse_camera(
    const YAML::Node& root, const std::filesystem::path& path) {
  mapping(root, path,
          {"calibration_status", "baseline_m", "image_width", "image_height",
           "camera_name", "camera_matrix", "distortion_model",
           "distortion_coefficients", "rectification_matrix",
           "projection_matrix"},
          {"rectified_output_correction"});
  CameraCalibrationDocument result;
  result.camera_name = root["camera_name"].as<std::string>();
  result.calibration_status = root["calibration_status"].as<std::string>();
  result.image_width = root["image_width"].as<std::uint32_t>();
  result.image_height = root["image_height"].as<std::uint32_t>();
  result.baseline_m = root["baseline_m"].as<double>();
  const auto distortion_model = root["distortion_model"].as<std::string>();
  if (result.camera_name.empty() || result.image_width == 0U ||
      result.image_height == 0U || !std::isfinite(result.baseline_m) ||
      result.baseline_m <= 0.0 ||
      (distortion_model != "plumb_bob" &&
       distortion_model != "rational_polynomial")) {
    throw std::invalid_argument(path.string() +
                                ": invalid camera identity, geometry, or model");
  }
  result.camera_matrix =
      fixed_matrix<9>(root["camera_matrix"], path, "camera_matrix", 3U, 3U);
  result.rectification_matrix = fixed_matrix<9>(
      root["rectification_matrix"], path, "rectification_matrix", 3U, 3U);
  result.projection_matrix = fixed_matrix<12>(
      root["projection_matrix"], path, "projection_matrix", 3U, 4U);
  if (root["rectified_output_correction"]) {
    result.rectified_output_correction = fixed_matrix<9>(
        root["rectified_output_correction"], path,
        "rectified_output_correction", 3U, 3U);
    const auto& h = result.rectified_output_correction;
    const auto determinant =
        h[0] * (h[4] * h[8] - h[5] * h[7]) -
        h[1] * (h[3] * h[8] - h[5] * h[6]) +
        h[2] * (h[3] * h[7] - h[4] * h[6]);
    if (std::abs(determinant) < 1e-12) {
      throw std::invalid_argument(path.string() +
                                  ": rectified_output_correction is singular");
    }
    result.has_rectified_output_correction = true;
  }

  const auto distortion = root["distortion_coefficients"];
  if (!distortion.IsMap() || !distortion["rows"] || !distortion["cols"] ||
      !distortion["data"] || distortion.size() != 3U ||
      distortion["rows"].as<std::uint32_t>() != 1U ||
      (distortion["cols"].as<std::uint32_t>() != 4U &&
       distortion["cols"].as<std::uint32_t>() != 5U &&
       distortion["cols"].as<std::uint32_t>() != 8U &&
       distortion["cols"].as<std::uint32_t>() != 12U &&
       distortion["cols"].as<std::uint32_t>() != 14U) ||
      (distortion_model == "rational_polynomial" &&
       distortion["cols"].as<std::uint32_t>() != 8U) ||
      !distortion["data"].IsSequence() ||
      distortion["data"].size() != distortion["cols"].as<std::size_t>()) {
    throw std::invalid_argument(path.string() +
                                ": distortion_coefficients has an invalid shape");
  }
  result.distortion_coefficients.reserve(distortion["data"].size());
  for (const auto value : distortion["data"]) {
    const auto parsed = value.as<double>();
    if (!std::isfinite(parsed)) {
      throw std::invalid_argument(path.string() +
                                  ": distortion coefficient is non-finite");
    }
    result.distortion_coefficients.push_back(parsed);
  }
  return result;
}

[[nodiscard]] CalibrationAuthority authority(const std::string_view status) {
  if (status == "provisional_recalibration_required") {
    return CalibrationAuthority::kProvisionalRecalibrationRequired;
  }
  if (status == "user_validated") {
    return CalibrationAuthority::kUserValidated;
  }
  if (status == "factory_validated") {
    return CalibrationAuthority::kFactoryValidated;
  }
  throw std::invalid_argument("unknown calibration_status: " +
                              std::string(status));
}

[[nodiscard]] std::string authority_name(
    const CalibrationAuthority value) {
  switch (value) {
    case CalibrationAuthority::kProvisionalRecalibrationRequired:
      return "provisional_recalibration_required";
    case CalibrationAuthority::kUserValidated:
      return "user_validated";
    case CalibrationAuthority::kFactoryValidated:
      return "factory_validated";
  }
  throw std::invalid_argument("unknown calibration authority");
}

void require_nonempty_scalar(const YAML::Node& manifest, const char* key,
                             const std::filesystem::path& path) {
  if (!manifest[key] || !manifest[key].IsScalar() ||
      manifest[key].as<std::string>().empty()) {
    throw std::invalid_argument(path.string() + ": missing bundle field '" +
                                key + "'");
  }
}

template <std::size_t Rows, std::size_t Cols>
[[nodiscard]] cv::Mat matrix(const std::array<double, Rows * Cols>& values) {
  cv::Mat result(static_cast<int>(Rows), static_cast<int>(Cols), CV_64F);
  std::copy(values.begin(), values.end(), result.ptr<double>());
  return result;
}

[[nodiscard]] RectificationMap map_for(
    const CameraCalibrationDocument& camera) {
  const auto distortion = cv::Mat(camera.distortion_coefficients, true).reshape(1, 1);
  cv::Mat x;
  cv::Mat y;
  cv::initUndistortRectifyMap(
      matrix<3, 3>(camera.camera_matrix), distortion,
      matrix<3, 3>(camera.rectification_matrix),
      matrix<3, 4>(camera.projection_matrix)(cv::Rect(0, 0, 3, 3)),
      cv::Size(static_cast<int>(camera.image_width),
               static_cast<int>(camera.image_height)),
      CV_32FC1, x, y);
  if (camera.has_rectified_output_correction) {
    cv::Mat corrected_x;
    cv::Mat corrected_y;
    const auto output_size = cv::Size(static_cast<int>(camera.image_width),
                                      static_cast<int>(camera.image_height));
    const auto correction = matrix<3, 3>(camera.rectified_output_correction);
    // warpPerspective evaluates dst(q) = src(H^-1 q), which composes the
    // nominal output-to-source maps with the forward output correction H.
    cv::warpPerspective(x, corrected_x, correction, output_size,
                        cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(-1));
    cv::warpPerspective(y, corrected_y, correction, output_size,
                        cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(-1));
    x = std::move(corrected_x);
    y = std::move(corrected_y);
  }
  RectificationMap result;
  result.width = camera.image_width;
  result.height = camera.image_height;
  result.source_x.resize(static_cast<std::size_t>(camera.image_width) *
                         camera.image_height);
  result.source_y.resize(result.source_x.size());
  std::copy(x.ptr<float>(), x.ptr<float>() + result.source_x.size(),
            result.source_x.begin());
  std::copy(y.ptr<float>(), y.ptr<float>() + result.source_y.size(),
            result.source_y.begin());
  return result;
}

[[nodiscard]] bool near(const double left, const double right) {
  return std::abs(left - right) <=
         std::max(1e-9, std::max(std::abs(left), std::abs(right)) * 1e-6);
}

}  // namespace

PreparedStereoCalibration load_and_prepare_stereo_calibration(
    const std::filesystem::path& left_path,
    const std::filesystem::path& right_path,
    const std::uint32_t matching_width,
    const std::uint32_t matching_height) {
  const auto left_bytes = read_file(left_path);
  const auto right_bytes = read_file(right_path);
  PreparedStereoCalibration result;
  try {
    result.left = parse_camera(YAML::Load(left_bytes), left_path);
    result.right = parse_camera(YAML::Load(right_bytes), right_path);
  } catch (const YAML::Exception& error) {
    throw std::invalid_argument("calibration YAML error: " +
                                std::string(error.what()));
  }
  if (result.left.camera_name == result.right.camera_name ||
      result.left.image_width != result.right.image_width ||
      result.left.image_height != result.right.image_height ||
      result.left.calibration_status != result.right.calibration_status ||
      !near(result.left.baseline_m, result.right.baseline_m)) {
    throw std::invalid_argument(
        "left/right calibration documents are not one coherent stereo bundle");
  }
  if (matching_width == 0U || matching_height == 0U ||
      matching_width > result.left.image_width ||
      matching_height > result.left.image_height) {
    throw std::invalid_argument("matching grid is outside calibration geometry");
  }

  std::string identity_material("nuvo-stereo-calibration-v1\n");
  identity_material += std::to_string(left_bytes.size()) + "\n" + left_bytes;
  identity_material += std::to_string(right_bytes.size()) + "\n" + right_bytes;
  result.identity = calibration_identity_from_sha256(
      config::sha256_hex(identity_material));
  result.authority = authority(result.left.calibration_status);
  const auto reference =
      make_reference_grid(result.left.image_width, result.left.image_height);
  result.image_view = make_rectified_calibration(
      result.identity, result.authority, reference,
      result.left.projection_matrix, result.right.projection_matrix);
  if (!near(result.image_view.baseline_m, result.left.baseline_m)) {
    throw std::invalid_argument(
        "calibration baseline disagrees with projection matrices");
  }
  result.matching_view = resample_rectified_calibration(
      result.image_view,
      make_resampled_grid(reference, matching_width, matching_height));
  result.maps.left = map_for(result.left);
  result.maps.right = map_for(result.right);
  if (!is_valid(result.maps)) {
    throw std::runtime_error("generated rectification maps are invalid");
  }
  return result;
}

PreparedStereoCalibration load_and_prepare_active_calibration(
    const std::filesystem::path& bundle_path,
    const CalibrationAuthority expected_authority,
    const std::uint32_t matching_width,
    const std::uint32_t matching_height) {
  const auto manifest_path = bundle_path / "manifest.json";
  YAML::Node manifest;
  try {
    manifest = YAML::Load(read_file(manifest_path));
  } catch (const YAML::Exception& error) {
    throw std::invalid_argument("calibration manifest error: " +
                                std::string(error.what()));
  }
  if (!manifest.IsMap() || !manifest["schema_version"] ||
      manifest["schema_version"].as<std::uint32_t>() != 1U ||
      !manifest["assets"] || !manifest["assets"].IsMap() ||
      manifest["assets"].size() != 3U) {
    throw std::invalid_argument(manifest_path.string() +
                                ": invalid bundle schema or asset manifest");
  }
  for (const auto* key : {"bundle_id", "unit_id", "left_camera_id",
                          "right_camera_id", "preset_id", "authority"}) {
    require_nonempty_scalar(manifest, key, manifest_path);
  }
  const auto bundle_id = manifest["bundle_id"].as<std::string>();
  (void)calibration_identity_from_sha256(bundle_id);
  const auto canonical = std::filesystem::canonical(bundle_path);
  if (canonical.filename() != bundle_id) {
    throw std::invalid_argument(
        "activated calibration directory does not match its bundle identity");
  }
  const auto expected_name = authority_name(expected_authority);
  if (manifest["authority"].as<std::string>() != expected_name) {
    throw std::invalid_argument(
        "activated calibration namespace and authority disagree");
  }
  for (const auto* name : {"front_left.yaml", "front_right.yaml",
                           "validation.json"}) {
    const auto expected = manifest["assets"][name];
    if (!expected || !expected.IsScalar()) {
      throw std::invalid_argument(manifest_path.string() +
                                  ": missing asset hash for " + name);
    }
    const auto actual = config::sha256_hex(read_file(bundle_path / name));
    if (actual != expected.as<std::string>()) {
      throw std::invalid_argument("activated calibration asset hash mismatch: " +
                                  std::string(name));
    }
  }
  YAML::Node validation;
  try {
    validation = YAML::Load(read_file(bundle_path / "validation.json"));
  } catch (const YAML::Exception& error) {
    throw std::invalid_argument("calibration validation record error: " +
                                std::string(error.what()));
  }
  if (!validation.IsMap() || !validation["status"] ||
      validation["status"].as<std::string>() != "pass") {
    throw std::invalid_argument(
        "activated calibration validation record did not pass");
  }
  auto result = load_and_prepare_stereo_calibration(
      bundle_path / "front_left.yaml", bundle_path / "front_right.yaml",
      matching_width, matching_height);
  if (result.authority != expected_authority) {
    throw std::invalid_argument(
        "calibration documents and bundle authority disagree");
  }
  result.bundle_id = bundle_id;
  result.unit_id = manifest["unit_id"].as<std::string>();
  result.left_camera_id = manifest["left_camera_id"].as<std::string>();
  result.right_camera_id = manifest["right_camera_id"].as<std::string>();
  result.preset_id = manifest["preset_id"].as<std::string>();
  result.identity = calibration_identity_from_sha256(bundle_id);
  result.image_view.identity = result.identity;
  result.matching_view.identity = result.identity;
  return result;
}

}  // namespace depthd
