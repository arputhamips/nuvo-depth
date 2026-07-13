#pragma once

#include <span>
#include <vector>

#include "depthd/semantic_types.hpp"

namespace depthd {

struct SemanticFusionConfig {
  float min_range_m{0.50F};
  float max_range_m{6.00F};
  std::uint32_t sample_step_px{2};
  float inner_box_fraction{0.60F};
  std::uint32_t min_valid_samples{12};
  float min_valid_fraction{0.05F};
  float max_range_stddev_m{0.50F};
  float living_stop_range_m{3.00F};
  float living_stop_bearing_abs_rad{0.90F};
  bool stop_on_unranged_living{true};
};

class SemanticDepthFusion {
 public:
  explicit SemanticDepthFusion(SemanticFusionConfig config = {});

  [[nodiscard]] FusedSemanticObject fuse(
      const SemanticObservation& observation,
      const DisparityFrameView& disparity,
      const StereoCameraModel& camera,
      const RigidTransform& optical_to_target) const;

  [[nodiscard]] std::vector<FusedSemanticObject> fuse_all(
      std::span<const SemanticObservation> observations,
      const DisparityFrameView& disparity,
      const StereoCameraModel& camera,
      const RigidTransform& optical_to_target) const;

  [[nodiscard]] SemanticHazardDecision evaluate_hazard(
      std::span<const FusedSemanticObject> objects,
      bool perception_stale = false) const;

 private:
  SemanticFusionConfig config_{};
};

}  // namespace depthd
