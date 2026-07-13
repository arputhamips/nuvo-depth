#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "depthd/obstacle_cloud.hpp"
#include "depthd/result_ring.hpp"

namespace depthd {

struct NativeOutputConfig {
  ResultRingConfig ring{};
  CompactObstacleCloudConfig obstacle_cloud{};
  StereoCameraModel stereo_camera{};
  RigidTransform optical_to_obstacle_frame{};
};

struct NativeOutputFrameView {
  std::int64_t capture_realtime_ns{};
  ipc::HealthState health{ipc::HealthState::kOk};
  DisparityFrameView disparity{};
  std::span<const std::uint8_t> left_rectified_mono{};
  std::span<const std::uint8_t> left_rectified_jpeg{};
};

struct NativeOutputStats {
  std::uint64_t sequence{};
  std::uint32_t obstacle_point_count{};
  double obstacle_cloud_ms{};
  double result_ring_publish_ms{};
  double total_ms{};
  CompactObstacleCloudStats obstacle_cloud{};
};

class NativeOutputPublisher {
 public:
  explicit NativeOutputPublisher(NativeOutputConfig config);
  ~NativeOutputPublisher();

  NativeOutputPublisher(NativeOutputPublisher&&) noexcept;
  NativeOutputPublisher& operator=(NativeOutputPublisher&&) noexcept;
  NativeOutputPublisher(const NativeOutputPublisher&) = delete;
  NativeOutputPublisher& operator=(const NativeOutputPublisher&) = delete;

  [[nodiscard]] NativeOutputStats publish(
      const NativeOutputFrameView& frame);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] RigidTransform make_nuvo_left_optical_to_base_transform()
    noexcept;

}  // namespace depthd
