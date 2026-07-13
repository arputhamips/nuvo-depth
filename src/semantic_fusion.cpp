#include "depthd/semantic_fusion.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace depthd {
namespace {

Point3f transform_point(const RigidTransform& transform, const float x,
                        const float y, const float z) noexcept {
  const auto& m = transform.matrix;
  return {
      static_cast<float>(m[0] * x + m[1] * y + m[2] * z + m[3]),
      static_cast<float>(m[4] * x + m[5] * y + m[6] * z + m[7]),
      static_cast<float>(m[8] * x + m[9] * y + m[10] * z + m[11]),
  };
}

float median(std::vector<float>& values) {
  const auto middle = values.begin() +
      static_cast<std::ptrdiff_t>(values.size() / 2U);
  std::nth_element(values.begin(), middle, values.end());
  float result = *middle;
  if ((values.size() & 1U) == 0U) {
    const auto lower = std::max_element(values.begin(), middle);
    result = (*lower + result) * 0.5F;
  }
  return result;
}

bool living(const SemanticClass semantic_class) noexcept {
  return semantic_class == SemanticClass::kHuman ||
         semantic_class == SemanticClass::kAnimal;
}

}  // namespace

SemanticDepthFusion::SemanticDepthFusion(SemanticFusionConfig config)
    : config_(config) {
  if (!(config_.min_range_m >= 0.0F &&
        config_.max_range_m > config_.min_range_m) ||
      config_.sample_step_px == 0U ||
      !(config_.inner_box_fraction > 0.0F &&
        config_.inner_box_fraction <= 1.0F) ||
      config_.min_valid_samples == 0U ||
      !(config_.min_valid_fraction >= 0.0F &&
        config_.min_valid_fraction <= 1.0F) ||
      config_.max_range_stddev_m < 0.0F ||
      config_.living_stop_range_m <= 0.0F ||
      config_.living_stop_bearing_abs_rad <= 0.0F) {
    throw std::invalid_argument("invalid semantic depth-fusion configuration");
  }
}

FusedSemanticObject SemanticDepthFusion::fuse(
    const SemanticObservation& observation,
    const DisparityFrameView& disparity,
    const StereoCameraModel& camera,
    const RigidTransform& optical_to_target) const {
  FusedSemanticObject output;
  output.observation = observation;
  output.range_m = std::numeric_limits<float>::quiet_NaN();
  output.bearing_rad = std::numeric_limits<float>::quiet_NaN();
  output.range_stddev_m = std::numeric_limits<float>::quiet_NaN();

  if (disparity.data == nullptr || disparity.width == 0U ||
      disparity.height == 0U ||
      disparity.stride_elements < disparity.width || camera.fx <= 0.0 ||
      camera.fy <= 0.0 || camera.baseline_m <= 0.0) {
    return output;
  }

  const auto full_valid_width = disparity.valid_width == 0U
      ? disparity.width : disparity.valid_width;
  const auto full_valid_height = disparity.valid_height == 0U
      ? disparity.height : disparity.valid_height;
  const auto valid_right = std::min(
      disparity.width, disparity.valid_x_offset + full_valid_width);
  const auto valid_bottom = std::min(
      disparity.height, disparity.valid_y_offset + full_valid_height);

  const float center_x = (observation.box.x_min + observation.box.x_max) * 0.5F;
  const float center_y = (observation.box.y_min + observation.box.y_max) * 0.5F;
  const float half_width = std::max(
      0.0F, (observation.box.x_max - observation.box.x_min) *
                config_.inner_box_fraction * 0.5F);
  const float half_height = std::max(
      0.0F, (observation.box.y_max - observation.box.y_min) *
                config_.inner_box_fraction * 0.5F);
  const auto left = static_cast<std::uint32_t>(std::clamp(
      std::floor(center_x - half_width), 0.0F,
      static_cast<float>(disparity.width)));
  const auto top = static_cast<std::uint32_t>(std::clamp(
      std::floor(center_y - half_height), 0.0F,
      static_cast<float>(disparity.height)));
  const auto right = static_cast<std::uint32_t>(std::clamp(
      std::ceil(center_x + half_width), 0.0F,
      static_cast<float>(disparity.width)));
  const auto bottom = static_cast<std::uint32_t>(std::clamp(
      std::ceil(center_y + half_height), 0.0F,
      static_cast<float>(disparity.height)));

  const auto sample_left = std::max(left, disparity.valid_x_offset);
  const auto sample_top = std::max(top, disparity.valid_y_offset);
  const auto sample_right = std::min(right, valid_right);
  const auto sample_bottom = std::min(bottom, valid_bottom);
  if (sample_left >= sample_right || sample_top >= sample_bottom) {
    return output;
  }

  std::vector<float> xs;
  std::vector<float> ys;
  std::vector<float> zs;
  std::vector<float> ranges;
  std::uint32_t sampled = 0U;
  for (auto v = sample_top; v < sample_bottom; v += config_.sample_step_px) {
    for (auto u = sample_left; u < sample_right; u += config_.sample_step_px) {
      ++sampled;
      const float d = disparity.data[
          static_cast<std::size_t>(v) * disparity.stride_elements + u];
      if (!std::isfinite(d) || d <= disparity.min_disparity ||
          (disparity.max_disparity > disparity.min_disparity &&
           d > disparity.max_disparity)) {
        continue;
      }
      const float optical_z =
          static_cast<float>(camera.fx * camera.baseline_m) / d;
      if (!std::isfinite(optical_z) || optical_z < config_.min_range_m ||
          optical_z > config_.max_range_m) {
        continue;
      }
      const float optical_x =
          (static_cast<float>(u) - static_cast<float>(camera.cx)) *
          optical_z / static_cast<float>(camera.fx);
      const float optical_y =
          (static_cast<float>(v) - static_cast<float>(camera.cy)) *
          optical_z / static_cast<float>(camera.fy);
      const auto point = transform_point(
          optical_to_target, optical_x, optical_y, optical_z);
      const float range = std::hypot(point.x, point.y);
      if (!std::isfinite(range)) {
        continue;
      }
      xs.push_back(point.x);
      ys.push_back(point.y);
      zs.push_back(point.z);
      ranges.push_back(range);
    }
  }

  output.valid_samples = static_cast<std::uint32_t>(ranges.size());
  output.valid_fraction = sampled == 0U
      ? 0.0F
      : static_cast<float>(ranges.size()) / static_cast<float>(sampled);
  if (ranges.size() < config_.min_valid_samples ||
      output.valid_fraction < config_.min_valid_fraction) {
    return output;
  }

  const float mean = std::accumulate(ranges.begin(), ranges.end(), 0.0F) /
                     static_cast<float>(ranges.size());
  float squared_error = 0.0F;
  for (const float range : ranges) {
    const float error = range - mean;
    squared_error += error * error;
  }
  output.range_stddev_m = std::sqrt(
      squared_error / static_cast<float>(ranges.size()));
  if (output.range_stddev_m > config_.max_range_stddev_m) {
    return output;
  }

  output.point = {median(xs), median(ys), median(zs)};
  output.range_m = std::hypot(output.point.x, output.point.y);
  output.bearing_rad = std::atan2(output.point.y, output.point.x);
  output.range_valid = std::isfinite(output.range_m) &&
                       output.range_m >= config_.min_range_m &&
                       output.range_m <= config_.max_range_m;
  return output;
}

std::vector<FusedSemanticObject> SemanticDepthFusion::fuse_all(
    const std::span<const SemanticObservation> observations,
    const DisparityFrameView& disparity,
    const StereoCameraModel& camera,
    const RigidTransform& optical_to_target) const {
  std::vector<FusedSemanticObject> output;
  output.reserve(observations.size());
  for (const auto& observation : observations) {
    output.push_back(fuse(observation, disparity, camera, optical_to_target));
  }
  return output;
}

SemanticHazardDecision SemanticDepthFusion::evaluate_hazard(
    const std::span<const FusedSemanticObject> objects,
    const bool perception_stale) const {
  SemanticHazardDecision decision;
  decision.perception_stale = perception_stale;
  decision.fail_closed_required = perception_stale;
  if (perception_stale) {
    decision.active = true;
    decision.reason = "perception_stale_fail_closed";
    return decision;
  }
  for (const auto& object : objects) {
    if (!living(object.observation.semantic_class)) {
      continue;
    }
    const bool unranged_stop = !object.range_valid &&
                               config_.stop_on_unranged_living;
    const bool ranged_stop = object.range_valid &&
        object.range_m <= config_.living_stop_range_m &&
        std::abs(object.bearing_rad) <=
            config_.living_stop_bearing_abs_rad;
    if (!unranged_stop && !ranged_stop) {
      continue;
    }
    decision.active = true;
    decision.fail_closed_required = unranged_stop;
    decision.source_observation_id = object.observation.observation_id;
    decision.semantic_class = object.observation.semantic_class;
    decision.range_m = object.range_m;
    decision.bearing_rad = object.bearing_rad;
    decision.reason = unranged_stop ? "living_obstacle_without_range"
                                    : "living_obstacle_in_stop_zone";
    return decision;
  }
  decision.reason = "clear";
  return decision;
}

}  // namespace depthd
