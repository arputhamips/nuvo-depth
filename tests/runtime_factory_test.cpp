#include "depthd/runtime_factory.hpp"

#include <cassert>
#include <filesystem>
#include <stdexcept>

namespace {

template <typename Operation>
void expect_rejection(Operation operation, const char* message) {
  try {
    operation();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(message);
}

}  // namespace

int main() {
  const auto source = std::filesystem::path(DEPTHD_SOURCE_DIR);
  auto loaded = depthd::config::load(
      source / "config/nuvo_pi5_depth_100mm.yaml");
  const auto calibration = depthd::load_and_prepare_stereo_calibration(
      source / "config/calibration/nuvo_depth_100mm/front_left.yaml",
      source / "config/calibration/nuvo_depth_100mm/front_right.yaml", 728U,
      544U);
  const auto identity = depthd::make_runtime_identity(loaded);
  const auto provisioned = depthd::make_runtime_identity(
      loaded, "12345678-1234-4234-9234-123456789abc");
  if (provisioned.device_uuid[0] != 0x12U ||
      provisioned.device_uuid[6] != 0x42U) {
    throw std::runtime_error("provisioned device UUID was not preserved");
  }
  bool rejected = false;
  try {
    (void)depthd::make_native_pipeline(loaded.effective, calibration, identity);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  if (!rejected) {
    throw std::runtime_error("provisional calibration was accepted implicitly");
  }

  depthd::RuntimeFactoryOptions engineering;
  engineering.allow_provisional_depth_for_engineering = true;
  auto pipeline = depthd::make_native_pipeline(
      loaded.effective, calibration, identity, engineering);
  assert(pipeline != nullptr);
  // Effective configurations can also be constructed by callers. Rejecting
  // these values proves the factory forwards mode and median to the matcher
  // instead of silently retaining the backend's half-grid 3-way defaults.
  auto invalid_mode = loaded.effective;
  invalid_mode.stereo.mode = "unknown";
  expect_rejection([&] {
    (void)depthd::make_native_pipeline(invalid_mode, calibration, identity, engineering);
  }, "runtime factory ignored stereo aggregation mode");
  auto invalid_median = loaded.effective;
  invalid_median.stereo.median_filter_size = 4U;
  expect_rejection([&] {
    (void)depthd::make_native_pipeline(invalid_median, calibration, identity, engineering);
  }, "runtime factory ignored stereo smoothing");

  // Simulate a coherent measured rig with a 1 px focal and 1 mm baseline
  // difference from the shipped nominal seed. Both validated authorities must
  // use those projections without requiring the daemon YAML to be rewritten.
  auto measured = calibration;
  auto left_projection = calibration.image_view.left_projection;
  auto right_projection = calibration.image_view.right_projection;
  left_projection[0] += 1.0;
  left_projection[5] += 1.0;
  right_projection[0] = left_projection[0];
  right_projection[5] = left_projection[5];
  right_projection[3] = -left_projection[0] * 0.101;
  for (const auto authority : {depthd::CalibrationAuthority::kUserValidated,
                                depthd::CalibrationAuthority::kFactoryValidated}) {
    measured.authority = authority;
    measured.image_view = depthd::make_rectified_calibration(
        measured.identity, authority, calibration.image_view.grid,
        left_projection, right_projection);
    measured.matching_view = depthd::resample_rectified_calibration(
        measured.image_view, calibration.matching_view.grid);
    auto calibrated_pipeline = depthd::make_native_pipeline(
        loaded.effective, measured, identity);
    assert(calibrated_pipeline != nullptr);
  }

  auto wrong_geometry = loaded.effective;
  ++wrong_geometry.capture.width;
  expect_rejection([&] {
    (void)depthd::make_native_pipeline(wrong_geometry, measured, identity);
  }, "validated calibration accepted a different capture grid");
  auto wrong_identity = measured;
  wrong_identity.identity.sha256[0] ^= 1U;
  expect_rejection([&] {
    (void)depthd::make_native_pipeline(loaded.effective, wrong_identity, identity);
  }, "validated calibration accepted incoherent content identity");
  auto wrong_authority = measured;
  wrong_authority.authority = depthd::CalibrationAuthority::kUserValidated;
  expect_rejection([&] {
    (void)depthd::make_native_pipeline(loaded.effective, wrong_authority, identity);
  }, "validated calibration accepted incoherent authority");
  measured.authority =
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired;
  measured.image_view.authority = measured.authority;
  measured.matching_view.authority = measured.authority;
  expect_rejection([&] {
    (void)depthd::make_native_pipeline(loaded.effective, measured, identity,
                                     engineering);
  }, "provisional seed geometry disagreement was accepted");
}
