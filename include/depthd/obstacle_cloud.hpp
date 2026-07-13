#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace depthd {

struct Point3f {
  float x{};
  float y{};
  float z{};
};

struct RigidTransform {
  // Row-major homogeneous transform from the disparity optical frame to the
  // requested output frame.
  std::array<double, 16> matrix{
      1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
};

struct StereoCameraModel {
  std::uint32_t width{728};
  std::uint32_t height{544};
  double fx{493.542046188};
  double fy{493.542046188};
  double cx{348.240810394};
  double cy{266.509811401};
  double baseline_m{0.161458780};
};

struct DisparityFrameView {
  const float* data{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t stride_elements{};
  std::uint32_t valid_x_offset{};
  std::uint32_t valid_y_offset{};
  // A zero valid width or height means the complete image.
  std::uint32_t valid_width{};
  std::uint32_t valid_height{};
  float min_disparity{std::numeric_limits<float>::quiet_NaN()};
  float max_disparity{std::numeric_limits<float>::quiet_NaN()};
};

struct CompactObstacleCloudConfig {
  std::uint32_t roi_y_min{96};
  std::uint32_t roi_y_max{420};
  std::uint32_t row_step{2};
  std::uint32_t col_step{2};
  double depth_min_m{0.75};
  double depth_max_m{6.0};
  double min_height_m{0.18};
  double max_height_m{1.20};
  double angle_min_rad{-0.70};
  double angle_max_rad{0.70};
  double voxel_leaf_size_m{0.05};
  std::uint32_t min_points_per_voxel{5};
  std::uint32_t min_cluster_voxels{12};
  std::uint32_t min_cluster_raw_points{60};
  // Fixed-capacity scratch storage prevents steady-state heap allocation.
  std::uint32_t max_voxels{32768};
};

struct CompactObstacleCloudStats {
  std::size_t sampled_pixels{};
  std::size_t valid_disparities{};
  std::size_t accepted_raw_points{};
  std::size_t accumulated_voxels{};
  std::size_t supported_voxels{};
  std::size_t accepted_clusters{};
  std::size_t rejected_clusters{};
  std::size_t rejected_nonfinite_disparity{};
  std::size_t rejected_min_disparity{};
  std::size_t rejected_max_disparity{};
  std::size_t rejected_depth{};
  std::size_t rejected_height{};
  std::size_t rejected_behind{};
  std::size_t rejected_angle{};
  std::size_t rejected_range{};
  std::size_t rejected_voxel_support{};
  std::size_t rejected_cluster_voxels{};
  std::size_t voxel_capacity_overflow{};
};

struct CompactObstacleCloudResult {
  // Valid until the next generate() call on the same generator.
  std::span<const Point3f> points;
  CompactObstacleCloudStats stats;
};

class CompactObstacleCloudGenerator {
 public:
  explicit CompactObstacleCloudGenerator(CompactObstacleCloudConfig config);
  ~CompactObstacleCloudGenerator();

  CompactObstacleCloudGenerator(CompactObstacleCloudGenerator&&) noexcept;
  CompactObstacleCloudGenerator& operator=(
      CompactObstacleCloudGenerator&&) noexcept;
  CompactObstacleCloudGenerator(const CompactObstacleCloudGenerator&) = delete;
  CompactObstacleCloudGenerator& operator=(
      const CompactObstacleCloudGenerator&) = delete;

  [[nodiscard]] const CompactObstacleCloudConfig& config() const noexcept;
  [[nodiscard]] CompactObstacleCloudResult generate(
      const DisparityFrameView& disparity,
      const StereoCameraModel& camera,
      const RigidTransform& optical_to_target);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace depthd
