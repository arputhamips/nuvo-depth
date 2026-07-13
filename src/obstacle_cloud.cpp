#include "depthd/obstacle_cloud.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace depthd {
namespace {

constexpr std::size_t kNotFound = std::numeric_limits<std::size_t>::max();

struct VoxelKey {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t z{};

  bool operator==(const VoxelKey&) const = default;
};

struct VoxelSlot {
  VoxelKey key;
  double sum_x{};
  double sum_y{};
  double sum_z{};
  std::uint32_t raw_count{};
  bool occupied{};
  bool supported{};
  bool visited{};
};

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
  value ^= value >> 30U;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27U;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

[[nodiscard]] std::size_t hash_key(const VoxelKey& key) noexcept {
  const auto x = static_cast<std::uint32_t>(key.x);
  const auto y = static_cast<std::uint32_t>(key.y);
  const auto z = static_cast<std::uint32_t>(key.z);
  return static_cast<std::size_t>(
      mix64(x) ^ std::rotl(mix64(y), 21) ^ std::rotl(mix64(z), 42));
}

[[nodiscard]] bool finite_positive(const double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}

void validate_config(const CompactObstacleCloudConfig& config) {
  if (config.roi_y_max <= config.roi_y_min || config.row_step == 0 ||
      config.col_step == 0 || !finite_positive(config.depth_min_m) ||
      !finite_positive(config.depth_max_m) ||
      config.depth_max_m <= config.depth_min_m ||
      !std::isfinite(config.min_height_m) ||
      !std::isfinite(config.max_height_m) ||
      config.max_height_m <= config.min_height_m ||
      !std::isfinite(config.angle_min_rad) ||
      !std::isfinite(config.angle_max_rad) ||
      config.angle_max_rad <= config.angle_min_rad ||
      !finite_positive(config.voxel_leaf_size_m) ||
      config.min_points_per_voxel == 0 || config.min_cluster_voxels == 0 ||
      config.min_cluster_raw_points == 0 || config.max_voxels < 16) {
    throw std::invalid_argument("invalid compact obstacle cloud configuration");
  }
}

void validate_input(const DisparityFrameView& disparity,
                    const StereoCameraModel& camera,
                    const RigidTransform& transform) {
  if (disparity.data == nullptr || disparity.width == 0 ||
      disparity.height == 0 || disparity.stride_elements < disparity.width) {
    throw std::invalid_argument("invalid disparity frame view");
  }
  if (camera.width != disparity.width || camera.height != disparity.height ||
      !finite_positive(camera.fx) || !finite_positive(camera.fy) ||
      !std::isfinite(camera.cx) || !std::isfinite(camera.cy) ||
      !finite_positive(camera.baseline_m)) {
    throw std::invalid_argument("invalid stereo camera model");
  }
  if (!std::all_of(transform.matrix.begin(), transform.matrix.end(),
                   [](const double value) { return std::isfinite(value); })) {
    throw std::invalid_argument("invalid optical-to-target transform");
  }
}

[[nodiscard]] Point3f transform_point(const RigidTransform& transform,
                                      const double x, const double y,
                                      const double z) noexcept {
  const auto& m = transform.matrix;
  return {
      static_cast<float>(m[0] * x + m[1] * y + m[2] * z + m[3]),
      static_cast<float>(m[4] * x + m[5] * y + m[6] * z + m[7]),
      static_cast<float>(m[8] * x + m[9] * y + m[10] * z + m[11]),
  };
}

}  // namespace

class CompactObstacleCloudGenerator::Impl {
 public:
  explicit Impl(CompactObstacleCloudConfig config) : config_(config) {
    validate_config(config_);
    const std::size_t requested =
        static_cast<std::size_t>(config_.max_voxels) * 2U;
    table_size_ = std::bit_ceil(requested);
    slots_.resize(table_size_);
    used_slots_.reserve(config_.max_voxels);
    stack_.reserve(config_.max_voxels);
    cluster_.reserve(config_.max_voxels);
    points_.reserve(config_.max_voxels);
  }

  [[nodiscard]] const CompactObstacleCloudConfig& config() const noexcept {
    return config_;
  }

  CompactObstacleCloudResult generate(const DisparityFrameView& disparity,
                                      const StereoCameraModel& camera,
                                      const RigidTransform& transform) {
    validate_input(disparity, camera, transform);
    reset();
    CompactObstacleCloudStats stats;

    std::uint32_t x_begin = 0;
    std::uint32_t x_end = disparity.width;
    std::uint32_t y_begin = std::min(config_.roi_y_min, disparity.height);
    std::uint32_t y_end = std::min(config_.roi_y_max, disparity.height);
    if (disparity.valid_width > 0 && disparity.valid_height > 0) {
      x_begin = std::min(disparity.valid_x_offset, disparity.width);
      y_begin = std::max(
          y_begin, std::min(disparity.valid_y_offset, disparity.height));
      const auto valid_x_end =
          static_cast<std::uint64_t>(disparity.valid_x_offset) +
          disparity.valid_width;
      const auto valid_y_end =
          static_cast<std::uint64_t>(disparity.valid_y_offset) +
          disparity.valid_height;
      x_end = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(valid_x_end, disparity.width));
      y_end = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(valid_y_end, y_end));
    }
    if (x_end <= x_begin || y_end <= y_begin) {
      return {{}, stats};
    }

    const bool has_min = std::isfinite(disparity.min_disparity);
    const bool has_max = std::isfinite(disparity.max_disparity) &&
                         (!has_min ||
                          disparity.max_disparity > disparity.min_disparity);
    const double focal_baseline = camera.fx * camera.baseline_m;
    for (std::uint32_t v = y_begin; v < y_end; v += config_.row_step) {
      const float* row = disparity.data +
                         static_cast<std::size_t>(v) *
                             disparity.stride_elements;
      for (std::uint32_t u = x_begin; u < x_end; u += config_.col_step) {
        ++stats.sampled_pixels;
        const float value = row[u];
        if (!std::isfinite(value) || value <= 0.0F) {
          ++stats.rejected_nonfinite_disparity;
          continue;
        }
        if (has_min && value < disparity.min_disparity) {
          ++stats.rejected_min_disparity;
          continue;
        }
        if (has_max && value > disparity.max_disparity) {
          ++stats.rejected_max_disparity;
          continue;
        }
        ++stats.valid_disparities;

        const double depth = focal_baseline / static_cast<double>(value);
        if (!std::isfinite(depth) || depth < config_.depth_min_m ||
            depth > config_.depth_max_m) {
          ++stats.rejected_depth;
          continue;
        }
        const double optical_x =
            (static_cast<double>(u) - camera.cx) * depth / camera.fx;
        const double optical_y =
            (static_cast<double>(v) - camera.cy) * depth / camera.fy;
        const Point3f point =
            transform_point(transform, optical_x, optical_y, depth);
        if (point.z < config_.min_height_m ||
            point.z > config_.max_height_m) {
          ++stats.rejected_height;
          continue;
        }
        if (point.x <= 0.0F) {
          ++stats.rejected_behind;
          continue;
        }
        const double angle = std::atan2(point.y, point.x);
        if (angle < config_.angle_min_rad ||
            angle > config_.angle_max_rad) {
          ++stats.rejected_angle;
          continue;
        }
        const double range = std::hypot(point.x, point.y);
        if (!std::isfinite(range) || range < config_.depth_min_m ||
            range > config_.depth_max_m) {
          ++stats.rejected_range;
          continue;
        }
        ++stats.accepted_raw_points;
        const double inverse_leaf = 1.0 / config_.voxel_leaf_size_m;
        const VoxelKey key{
            static_cast<std::int32_t>(std::floor(point.x * inverse_leaf)),
            static_cast<std::int32_t>(std::floor(point.y * inverse_leaf)),
            static_cast<std::int32_t>(std::floor(point.z * inverse_leaf)),
        };
        VoxelSlot* slot = find_or_insert(key);
        if (slot == nullptr) {
          ++stats.voxel_capacity_overflow;
          continue;
        }
        slot->sum_x += point.x;
        slot->sum_y += point.y;
        slot->sum_z += point.z;
        ++slot->raw_count;
      }
    }

    stats.accumulated_voxels = used_slots_.size();
    for (const auto index : used_slots_) {
      auto& slot = slots_[index];
      slot.supported = slot.raw_count >= config_.min_points_per_voxel;
      if (slot.supported) {
        ++stats.supported_voxels;
      } else {
        ++stats.rejected_voxel_support;
      }
    }
    cluster_supported_voxels(stats);
    std::sort(points_.begin(), points_.end(),
              [](const Point3f& left, const Point3f& right) {
                if (left.x != right.x) {
                  return left.x < right.x;
                }
                if (left.y != right.y) {
                  return left.y < right.y;
                }
                return left.z < right.z;
              });
    return {std::span<const Point3f>(points_), stats};
  }

 private:
  void reset() {
    for (const auto index : used_slots_) {
      slots_[index] = VoxelSlot{};
    }
    used_slots_.clear();
    stack_.clear();
    cluster_.clear();
    points_.clear();
  }

  [[nodiscard]] std::size_t find_index(const VoxelKey& key) const noexcept {
    const std::size_t mask = table_size_ - 1U;
    std::size_t index = hash_key(key) & mask;
    for (std::size_t probe = 0; probe < table_size_; ++probe) {
      const auto& slot = slots_[index];
      if (!slot.occupied) {
        return kNotFound;
      }
      if (slot.key == key) {
        return index;
      }
      index = (index + 1U) & mask;
    }
    return kNotFound;
  }

  [[nodiscard]] VoxelSlot* find_or_insert(const VoxelKey& key) {
    const std::size_t mask = table_size_ - 1U;
    std::size_t index = hash_key(key) & mask;
    for (std::size_t probe = 0; probe < table_size_; ++probe) {
      auto& slot = slots_[index];
      if (!slot.occupied) {
        if (used_slots_.size() >= config_.max_voxels) {
          return nullptr;
        }
        slot.occupied = true;
        slot.key = key;
        used_slots_.push_back(index);
        return &slot;
      }
      if (slot.key == key) {
        return &slot;
      }
      index = (index + 1U) & mask;
    }
    return nullptr;
  }

  void cluster_supported_voxels(CompactObstacleCloudStats& stats) {
    for (const auto seed : used_slots_) {
      auto& seed_slot = slots_[seed];
      if (!seed_slot.supported || seed_slot.visited) {
        continue;
      }
      stack_.clear();
      cluster_.clear();
      std::size_t raw_points = 0;
      seed_slot.visited = true;
      stack_.push_back(seed);
      while (!stack_.empty()) {
        const std::size_t current = stack_.back();
        stack_.pop_back();
        cluster_.push_back(current);
        const auto& slot = slots_[current];
        raw_points += slot.raw_count;
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
          for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
              if (dx == 0 && dy == 0 && dz == 0) {
                continue;
              }
              const VoxelKey neighbor{slot.key.x + dx, slot.key.y + dy,
                                      slot.key.z + dz};
              const std::size_t neighbor_index = find_index(neighbor);
              if (neighbor_index == kNotFound) {
                continue;
              }
              auto& neighbor_slot = slots_[neighbor_index];
              if (!neighbor_slot.supported || neighbor_slot.visited) {
                continue;
              }
              neighbor_slot.visited = true;
              stack_.push_back(neighbor_index);
            }
          }
        }
      }
      if (cluster_.size() < config_.min_cluster_voxels ||
          raw_points < config_.min_cluster_raw_points) {
        ++stats.rejected_clusters;
        stats.rejected_cluster_voxels += cluster_.size();
        continue;
      }
      ++stats.accepted_clusters;
      for (const auto index : cluster_) {
        const auto& slot = slots_[index];
        const double inverse_count = 1.0 / slot.raw_count;
        points_.push_back(
            {static_cast<float>(slot.sum_x * inverse_count),
             static_cast<float>(slot.sum_y * inverse_count),
             static_cast<float>(slot.sum_z * inverse_count)});
      }
    }
  }

  CompactObstacleCloudConfig config_;
  std::size_t table_size_{};
  std::vector<VoxelSlot> slots_;
  std::vector<std::size_t> used_slots_;
  std::vector<std::size_t> stack_;
  std::vector<std::size_t> cluster_;
  std::vector<Point3f> points_;
};

CompactObstacleCloudGenerator::CompactObstacleCloudGenerator(
    CompactObstacleCloudConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

CompactObstacleCloudGenerator::~CompactObstacleCloudGenerator() = default;
CompactObstacleCloudGenerator::CompactObstacleCloudGenerator(
    CompactObstacleCloudGenerator&&) noexcept = default;
CompactObstacleCloudGenerator& CompactObstacleCloudGenerator::operator=(
    CompactObstacleCloudGenerator&&) noexcept = default;

const CompactObstacleCloudConfig& CompactObstacleCloudGenerator::config()
    const noexcept {
  return impl_->config();
}

CompactObstacleCloudResult CompactObstacleCloudGenerator::generate(
    const DisparityFrameView& disparity, const StereoCameraModel& camera,
    const RigidTransform& optical_to_target) {
  return impl_->generate(disparity, camera, optical_to_target);
}

}  // namespace depthd
