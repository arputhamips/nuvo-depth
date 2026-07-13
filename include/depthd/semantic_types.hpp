#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "depthd/obstacle_cloud.hpp"

namespace depthd {

enum class SemanticSourceKind : std::uint8_t {
  kInstance = 1,
  kDenseRegion = 2,
};

enum class SemanticClass : std::uint16_t {
  kUnknown = 0,
  kHuman = 1,
  kAnimal = 2,
  kVegetation = 3,
  kWall = 4,
  kFence = 5,
  kTerrain = 6,
  kGenericObstacle = 7,
};

enum class SemanticHealthState : std::uint32_t {
  kStarting = 0,
  kOk = 1,
  kDegraded = 2,
  kStale = 3,
  kFault = 4,
};

struct ImageBoundingBox {
  float x_min{};
  float y_min{};
  float x_max{};
  float y_max{};
};

struct SemanticObservation {
  SemanticSourceKind source_kind{SemanticSourceKind::kInstance};
  SemanticClass semantic_class{SemanticClass::kUnknown};
  std::uint32_t observation_id{};
  std::uint32_t model_class_id{};
  std::string model_label{};
  std::string model_name{};
  float confidence{};
  ImageBoundingBox box{};
  std::uint32_t pixel_count{};
};

struct SemanticBackendOutput {
  std::vector<SemanticObservation> instances{};
  std::vector<SemanticObservation> dense_regions{};
};

struct SemanticImageView {
  const std::uint8_t* bgr{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t stride_bytes{};
};

// The camera path can move its existing rectified color and disparity buffers
// into these shared owners. The semantic worker retains them without copying
// and never extends the lifetime of a camera request itself.
struct SemanticFrame {
  std::uint64_t source_sequence{};
  std::int64_t capture_realtime_ns{};
  std::int64_t submitted_steady_ns{};
  std::shared_ptr<const std::vector<std::uint8_t>> left_rectified_bgr{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t color_stride_bytes{};
  std::shared_ptr<const std::vector<float>> disparity{};
  std::size_t disparity_stride_elements{};
  std::uint32_t valid_window_x{};
  std::uint32_t valid_window_y{};
  std::uint32_t valid_window_width{};
  std::uint32_t valid_window_height{};
  float min_disparity{};
  float max_disparity{};
  StereoCameraModel camera{};
  RigidTransform optical_to_target{};
};

struct FusedSemanticObject {
  SemanticObservation observation{};
  bool range_valid{};
  Point3f point{};
  float range_m{};
  float bearing_rad{};
  float range_stddev_m{};
  float valid_fraction{};
  std::uint32_t valid_samples{};
};

struct SemanticHazardDecision {
  bool active{};
  bool perception_stale{};
  bool fail_closed_required{};
  std::uint32_t source_observation_id{};
  SemanticClass semantic_class{SemanticClass::kUnknown};
  float range_m{};
  float bearing_rad{};
  std::string reason{};
};

struct SemanticModelRun {
  std::string model_name{};
  std::string model_version{};
  std::string model_sha256{};
  SemanticHealthState health{SemanticHealthState::kStarting};
  std::uint64_t completed_runs{};
  std::uint64_t errors{};
  double latency_ms{};
  double inference_latency_ms{};
  double postprocess_latency_ms{};
};

struct SemanticFrameResult {
  std::uint64_t source_sequence{};
  std::int64_t capture_realtime_ns{};
  std::int64_t ready_realtime_ns{};
  SemanticHealthState health{SemanticHealthState::kStarting};
  std::vector<FusedSemanticObject> objects{};
  SemanticHazardDecision hazard{};
  std::vector<SemanticModelRun> models{};
  std::shared_ptr<const std::vector<std::uint8_t>> preview_jpeg{};
};

[[nodiscard]] const char* semantic_class_name(SemanticClass value) noexcept;

}  // namespace depthd
