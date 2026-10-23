#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "depthd/calibration_store.hpp"
#include "depthd/config.hpp"
#include "depthd/runtime.hpp"

namespace depthd {

struct PreparedOpenCvRectificationMaps;

struct RuntimeIdentity {
  std::array<std::uint8_t, 16> device_uuid{};
  std::array<std::uint8_t, 16> run_uuid{};
  std::uint64_t configuration_epoch{1U};
};

struct RuntimeFactoryOptions {
  bool image_only{};
  bool allow_provisional_depth_for_engineering{};
  std::string stereo_control_file{};
  float minimum_depth_m{0.05F};
  float maximum_depth_m{100.0F};
  std::shared_ptr<NativeConfigurationEpochs> configuration_epochs{};
  std::shared_ptr<const PreparedOpenCvRectificationMaps> cpu_rectification_maps{};
  std::size_t frame_pool_capacity{8U};
  std::size_t frame_pool_payload_budget_bytes{256U * 1024U * 1024U};
};

[[nodiscard]] RuntimeIdentity make_runtime_identity(
    const config::LoadedConfig& configuration,
    std::uint64_t configuration_epoch = 1U);

[[nodiscard]] RuntimeIdentity make_runtime_identity(
    const config::LoadedConfig& configuration, std::string_view device_uuid,
    std::uint64_t configuration_epoch = 1U);

[[nodiscard]] std::unique_ptr<NativeProcessingPipeline> make_native_pipeline(
    const config::EffectiveConfig& configuration,
    const PreparedStereoCalibration& calibration,
    const RuntimeIdentity& identity,
    const RuntimeFactoryOptions& options = {});

}  // namespace depthd
