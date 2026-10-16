#include "depthd/config.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef DEPTHD_SOURCE_DIR
#error "DEPTHD_SOURCE_DIR must identify the repository root"
#endif

namespace {

using depthd::config::ConfigError;
using depthd::config::ConfigErrorKind;

class TemporaryConfig final {
 public:
  explicit TemporaryConfig(const std::string_view contents) {
    static std::uint64_t counter{};
    path_ = std::filesystem::temp_directory_path() /
            ("depthd-config-test-" + std::to_string(++counter) + ".yaml");
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
      throw std::runtime_error("could not create temporary configuration");
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
      throw std::runtime_error("could not write temporary configuration");
    }
  }

  ~TemporaryConfig() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  TemporaryConfig(const TemporaryConfig&) = delete;
  TemporaryConfig& operator=(const TemporaryConfig&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("could not read " + path.string());
  }
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string replace_once(std::string input, const std::string_view from,
                         const std::string_view to) {
  const auto position = input.find(from);
  if (position == std::string::npos) {
    throw std::runtime_error("test mutation source was not found: " +
                             std::string(from));
  }
  input.replace(position, from.size(), to);
  return input;
}

bool expect_validation(const std::string_view contents,
                       const std::string_view expected_path) {
  const TemporaryConfig temporary(contents);
  try {
    (void)depthd::config::load(temporary.path());
  } catch (const ConfigError& error) {
    return error.kind() == ConfigErrorKind::kValidation &&
           error.path().find(expected_path) != std::string::npos;
  }
  return false;
}

bool expect_fixture_validation(const std::filesystem::path& path,
                               const std::string_view expected_path) {
  try {
    (void)depthd::config::load(path);
  } catch (const ConfigError& error) {
    return error.kind() == ConfigErrorKind::kValidation &&
           error.path().find(expected_path) != std::string::npos;
  }
  return false;
}

int fail(const int code, const std::string_view message) {
  std::cerr << "config_test: " << message << '\n';
  return code;
}

}  // namespace

int main() {
  const std::filesystem::path source_dir(DEPTHD_SOURCE_DIR);
  const auto active_path = source_dir / "config/nuvo_pi5_depth_100mm.yaml";
  const auto fixture_dir = source_dir / "tests/fixtures/config";
  const auto active_yaml = read_file(active_path);

  const auto loaded = depthd::config::load(active_path);
  if (loaded.effective.source_schema_version != 1U ||
      loaded.effective.capture.width != 728U ||
      loaded.effective.capture.height != 544U) {
    return fail(1, "active profile did not normalize to expected schema/geometry");
  }
  if (loaded.effective.rig.calibration_status !=
      depthd::config::CalibrationStatus::kProvisionalRecalibrationRequired) {
    return fail(2, "provisional calibration status was not preserved");
  }
  if (loaded.effective.legacy_ros.disparity_topic !=
      "/camera/front/disparity") {
    return fail(3, "legacy ROS settings were not isolated during normalization");
  }
  if (loaded.warnings.empty() ||
      loaded.warnings.front().path != "rig.calibration_status") {
    return fail(4, "provisional calibration warning was not emitted");
  }

  const auto first_dump = depthd::config::dump_json(loaded);
  const auto second_dump =
      depthd::config::dump_json(depthd::config::load(active_path));
  if (first_dump != second_dump || loaded.configuration_sha256.size() != 64U) {
    return fail(5, "effective output or digest is not deterministic");
  }
  if (first_dump.find("\"configuration_sha256\"") == std::string::npos ||
      first_dump.find("\"legacy_ros_adapter\"") == std::string::npos ||
      first_dump.find("provisional_recalibration_required") ==
          std::string::npos) {
    return fail(6, "effective JSON is missing provenance or compatibility data");
  }

  if (depthd::config::sha256_hex("") !=
          "e3b0c44298fc1c149afbf4c8996fb924"
          "27ae41e4649b934ca495991b7852b855" ||
      depthd::config::sha256_hex("abc") !=
          "ba7816bf8f01cfea414140de5dae2223"
          "b00361a396177a9cb410ff61f20015ad") {
    return fail(7, "SHA-256 implementation failed standard vectors");
  }

  const auto valid_fixture =
      depthd::config::load(fixture_dir / "valid-schema-v1.yaml");
  if (valid_fixture.configuration_sha256 != loaded.configuration_sha256) {
    return fail(8, "positive fixture differs from the active effective profile");
  }
  if (!expect_fixture_validation(fixture_dir / "duplicate-key.yaml",
                                 "schema_version")) {
    return fail(9, "duplicate key fixture did not fail at schema_version");
  }
  if (!expect_fixture_validation(fixture_dir / "unknown-key.yaml",
                                 "unexpected_root_key")) {
    return fail(10, "unknown key fixture did not fail at its path");
  }

  if (!expect_validation(
          replace_once(active_yaml, "schema_version: 1", "schema_version: 2"),
          "schema_version")) {
    return fail(11, "unsupported schema was accepted");
  }
  if (!expect_validation(replace_once(active_yaml, "  width: 728",
                                      "  width: not-a-number"),
                         "capture.width")) {
    return fail(12, "wrong scalar type was accepted");
  }
  if (!expect_validation(
          replace_once(active_yaml, "  baseline_m: 0.100",
                       "  baseline_m: .nan"),
          "rig.baseline_m")) {
    return fail(13, "nonfinite value was accepted");
  }
  if (!expect_validation(
          replace_once(active_yaml, "  width: 728", "  width: 0"),
          "capture.width/capture.height")) {
    return fail(14, "zero dimension was accepted");
  }
  if (!expect_validation(
          replace_once(active_yaml, "  width: 728", "  width: 20000"),
          "capture.width/capture.height")) {
    return fail(15, "oversized dimension was accepted");
  }

  auto overflow = replace_once(active_yaml, "  width: 728",
                               "  width: 4294967295");
  overflow = replace_once(overflow, "  height: 544", "  height: 4294967295");
  if (!expect_validation(overflow, "capture disparity frame bytes")) {
    return fail(16, "frame-storage overflow was not detected");
  }
  if (!expect_validation(
          replace_once(active_yaml, "  working_disparities: 128",
                       "  working_disparities: 63"),
          "stereo.working_disparities")) {
    return fail(17, "invalid SGBM disparity multiple was accepted");
  }
  if (!expect_validation(
          replace_once(active_yaml, "  max_disparity: 128",
                       "  max_disparity: 129"),
          "stereo.max_disparity")) {
    return fail(18, "inconsistent disparity range was accepted");
  }
  if (!expect_validation(
          replace_once(active_yaml, "metering_roi: [160, 96, 728, 420]",
                       "metering_roi: [160, 96, 729, 420]"),
          "capture.auto_exposure.metering_roi")) {
    return fail(19, "out-of-bounds metering ROI was accepted");
  }
  if (!expect_validation(
          replace_once(active_yaml, "  right_camera_index: 1",
                       "  right_camera_index: 0"),
          "capture.right_camera_index")) {
    return fail(20, "duplicate logical camera assignment was accepted");
  }
  if (!expect_validation(
          replace_once(active_yaml, "    min_shutter_us: 250",
                       "    min_shutter_us: 9000"),
          "capture.auto_exposure.min_shutter_us")) {
    return fail(21, "contradictory AE shutter range was accepted");
  }
  if (!expect_validation(
          replace_once(active_yaml, "    pulse_width_us: 8000",
                       "    pulse_width_us: 100000"),
          "capture.hardware_trigger.pulse_width_us")) {
    return fail(22, "trigger/cadence contradiction was accepted");
  }
  if (!expect_validation(
          replace_once(active_yaml, "  analogue_gain: 2.0",
                       "  analogue_gain: 4.0"),
          "capture.analogue_gain")) {
    return fail(23, "out-of-range initial gain was accepted");
  }

  try {
    (void)depthd::config::load(
        fixture_dir / "this-configuration-does-not-exist.yaml");
    return fail(24, "missing file was accepted");
  } catch (const ConfigError& error) {
    if (error.kind() != ConfigErrorKind::kIo) {
      return fail(25, "missing file did not produce an I/O error");
    }
  }

  for (const auto mode : {"sgbm_3way", "sgbm_standard", "sgbm_hh4", "sgbm_hh"}) {
    const TemporaryConfig temporary(
        replace_once(active_yaml, "mode: sgbm_3way", std::string("mode: ") + mode));
    if (depthd::config::load(temporary.path()).effective.stereo.mode != mode) {
      return fail(26, "supported aggregation mode was not preserved");
    }
  }
  if (!expect_validation(replace_once(active_yaml, "mode: sgbm_3way", "mode: mystery"),
                         "stereo.mode") ||
      !expect_validation(replace_once(active_yaml, "median_filter_size: 0",
                                      "median_filter_size: 4"),
                         "stereo.median_filter_size")) {
    return fail(27, "invalid aggregation or smoothing was accepted");
  }
  const TemporaryConfig without_median(
      replace_once(active_yaml, "  median_filter_size: 0\n", ""));
  if (depthd::config::load(without_median.path()).effective.stereo.median_filter_size != 0U) {
    return fail(28, "legacy schema-v1 smoothing default changed");
  }
  return 0;
}
