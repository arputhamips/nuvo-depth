#include "depthd/obstacle_cloud.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uint32_t kWidth = 728;
constexpr std::uint32_t kHeight = 544;

depthd::RigidTransform optical_to_base() {
  return {{0.0, 0.0, 1.0, 0.760, -1.0, 0.0, 0.0, 0.080,
           0.0, -1.0, 0.0, 0.180, 0.0, 0.0, 0.0, 1.0}};
}

depthd::DisparityFrameView view_of(const std::vector<float>& disparity) {
  return {disparity.data(), kWidth, kHeight, kWidth, 0, 0, 0, 0, 0.0F,
          128.0F};
}

bool near(const float value, const float expected, const float tolerance) {
  return std::abs(value - expected) <= tolerance;
}

}  // namespace

int main() {
  const float invalid = std::numeric_limits<float>::quiet_NaN();
  std::vector<float> disparity(static_cast<std::size_t>(kWidth) * kHeight,
                               invalid);
  depthd::StereoCameraModel camera;

  // A single center pixel with permissive support thresholds validates the
  // camera projection and REP-103 optical-to-base transform.
  depthd::CompactObstacleCloudConfig single_config;
  single_config.roi_y_min = 266;
  single_config.roi_y_max = 267;
  single_config.row_step = 1;
  single_config.col_step = 1;
  single_config.depth_min_m = 0.1;
  single_config.depth_max_m = 6.0;
  single_config.min_height_m = 0.0;
  single_config.max_height_m = 1.0;
  single_config.min_points_per_voxel = 1;
  single_config.min_cluster_voxels = 1;
  single_config.min_cluster_raw_points = 1;
  disparity[static_cast<std::size_t>(266) * kWidth + 348] =
      static_cast<float>(camera.fx * camera.baseline_m / 2.0);
  depthd::CompactObstacleCloudGenerator single_generator(single_config);
  auto result = single_generator.generate(view_of(disparity), camera,
                                           optical_to_base());
  if (result.points.size() != 1 || result.stats.accepted_raw_points != 1 ||
      result.stats.accepted_clusters != 1) {
    return 1;
  }
  if (!near(result.points[0].x, 2.760F, 0.002F) ||
      !near(result.points[0].y, 0.081F, 0.003F) ||
      !near(result.points[0].z, 0.182F, 0.003F)) {
    return 2;
  }

  // The production support thresholds must reject an isolated stereo island.
  depthd::CompactObstacleCloudGenerator production_generator({});
  result = production_generator.generate(view_of(disparity), camera,
                                          optical_to_base());
  if (!result.points.empty() || result.stats.rejected_voxel_support == 0) {
    return 3;
  }

  // A broad, textured-equivalent plane creates a supported connected cloud.
  const float plane_disparity =
      static_cast<float>(camera.fx * camera.baseline_m / 2.0);
  for (std::uint32_t y = 110; y < 260; ++y) {
    for (std::uint32_t x = 160; x < 568; ++x) {
      disparity[static_cast<std::size_t>(y) * kWidth + x] = plane_disparity;
    }
  }
  result = production_generator.generate(view_of(disparity), camera,
                                          optical_to_base());
  if (result.points.empty() || result.stats.accepted_clusters == 0 ||
      result.stats.voxel_capacity_overflow != 0 ||
      result.stats.supported_voxels != result.points.size()) {
    return 4;
  }
  for (std::size_t index = 0; index < result.points.size(); ++index) {
    const auto& point = result.points[index];
    if (point.x <= 0.0F || point.z < 0.18F || point.z > 1.20F ||
        std::hypot(point.x, point.y) < 0.75F ||
        std::hypot(point.x, point.y) > 6.0F) {
      return 5;
    }
    if (index > 0) {
      const auto& previous = result.points[index - 1];
      const bool ordered = previous.x < point.x ||
                           (previous.x == point.x && previous.y < point.y) ||
                           (previous.x == point.x && previous.y == point.y &&
                            previous.z <= point.z);
      if (!ordered) {
        return 6;
      }
    }
  }

  // The valid window must bound sampling before any pixel is projected.
  auto bounded = view_of(disparity);
  bounded.valid_x_offset = 700;
  bounded.valid_y_offset = 500;
  bounded.valid_width = 20;
  bounded.valid_height = 20;
  result = production_generator.generate(bounded, camera, optical_to_base());
  if (!result.points.empty() || result.stats.sampled_pixels != 0) {
    return 7;
  }

  // Invalid construction and programmer-owned input contracts fail loudly.
  try {
    auto invalid_config = depthd::CompactObstacleCloudConfig{};
    invalid_config.voxel_leaf_size_m = 0.0;
    depthd::CompactObstacleCloudGenerator invalid_generator(invalid_config);
    return 8;
  } catch (const std::invalid_argument&) {
  }
  try {
    auto invalid_view = view_of(disparity);
    invalid_view.stride_elements = kWidth - 1;
    const auto ignored =
        production_generator.generate(invalid_view, camera, optical_to_base());
    (void)ignored;
    return 9;
  } catch (const std::invalid_argument&) {
  }

  // Capacity exhaustion is explicit in diagnostics rather than reallocating
  // or silently overrunning bounded steady-state storage.
  std::fill(disparity.begin(), disparity.end(), invalid);
  auto capacity_config = single_config;
  capacity_config.roi_y_min = 266;
  capacity_config.roi_y_max = 267;
  capacity_config.max_voxels = 16;
  for (std::uint32_t x = 180; x < 580; x += 20) {
    disparity[static_cast<std::size_t>(266) * kWidth + x] = plane_disparity;
  }
  depthd::CompactObstacleCloudGenerator capacity_generator(capacity_config);
  result = capacity_generator.generate(view_of(disparity), camera,
                                       optical_to_base());
  if (result.stats.voxel_capacity_overflow == 0 ||
      result.stats.accumulated_voxels != 16) {
    return 10;
  }
  return 0;
}
