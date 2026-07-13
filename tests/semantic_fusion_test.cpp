#include "depthd/semantic_fusion.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

depthd::RigidTransform optical_to_base() {
  depthd::RigidTransform transform;
  transform.matrix = {
      0.0, 0.0, 1.0, 0.0,
      -1.0, 0.0, 0.0, 0.0,
      0.0, -1.0, 0.0, 0.0,
      0.0, 0.0, 0.0, 1.0,
  };
  return transform;
}

depthd::SemanticObservation observation(depthd::SemanticClass label) {
  depthd::SemanticObservation value;
  value.observation_id = 7U;
  value.semantic_class = label;
  value.model_label = depthd::semantic_class_name(label);
  value.box = {30.0F, 20.0F, 70.0F, 60.0F};
  value.confidence = 0.9F;
  return value;
}

}  // namespace

int main() {
  depthd::SemanticFusionConfig config;
  config.min_valid_samples = 8U;
  depthd::SemanticDepthFusion fusion(config);
  depthd::StereoCameraModel camera;
  camera.width = 100U;
  camera.height = 80U;
  camera.fx = 100.0;
  camera.fy = 100.0;
  camera.cx = 50.0;
  camera.cy = 40.0;
  camera.baseline_m = 0.20;

  std::vector<float> values(100U * 80U, 10.0F);
  depthd::DisparityFrameView disparity;
  disparity.data = values.data();
  disparity.width = 100U;
  disparity.height = 80U;
  disparity.stride_elements = 100U;
  disparity.min_disparity = 0.0F;
  disparity.max_disparity = 64.0F;

  const auto person = fusion.fuse(
      observation(depthd::SemanticClass::kHuman), disparity, camera,
      optical_to_base());
  expect(person.range_valid, "constant disparity produces a valid range");
  expect(std::abs(person.point.x - 2.0F) < 0.01F,
         "disparity projects to the expected forward distance");
  expect(std::abs(person.range_m - 2.0F) < 0.02F,
         "range is expressed in the target frame");
  expect(person.valid_samples >= config.min_valid_samples,
         "fusion records valid sample support");

  const std::vector<depthd::FusedSemanticObject> person_objects{person};
  const auto person_hazard = fusion.evaluate_hazard(person_objects);
  expect(person_hazard.active, "near human activates the safety hazard");
  expect(!person_hazard.fail_closed_required,
         "a ranged human uses the normal stop-zone policy");

  std::fill(values.begin(), values.end(),
            std::numeric_limits<float>::quiet_NaN());
  const auto unranged = fusion.fuse(
      observation(depthd::SemanticClass::kAnimal), disparity, camera,
      optical_to_base());
  expect(!unranged.range_valid, "invalid disparity remains explicitly unranged");
  const std::vector<depthd::FusedSemanticObject> unranged_objects{unranged};
  const auto unranged_hazard = fusion.evaluate_hazard(unranged_objects);
  expect(unranged_hazard.active && unranged_hazard.fail_closed_required,
         "an unranged living obstacle fails closed");

  const auto vegetation = fusion.fuse(
      observation(depthd::SemanticClass::kVegetation), disparity, camera,
      optical_to_base());
  const std::vector<depthd::FusedSemanticObject> vegetation_objects{vegetation};
  expect(!fusion.evaluate_hazard(vegetation_objects).active,
         "semantic vegetation does not replace geometry with a living hazard");
  const std::vector<depthd::FusedSemanticObject> empty;
  const auto stale = fusion.evaluate_hazard(empty, true);
  expect(stale.active && stale.perception_stale && stale.fail_closed_required,
         "stale perception is never represented as a fresh clear result");

  return failures == 0 ? 0 : 1;
}
