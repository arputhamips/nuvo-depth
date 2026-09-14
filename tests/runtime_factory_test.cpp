#include "depthd/runtime_factory.hpp"

#include <cassert>
#include <filesystem>
#include <stdexcept>

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
}
