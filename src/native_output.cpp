#include "depthd/native_output.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace depthd {
namespace {

using SteadyClock = std::chrono::steady_clock;

double elapsed_ms(const SteadyClock::time_point start,
                  const SteadyClock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

std::int64_t realtime_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

RigidTransform make_nuvo_left_optical_to_base_transform() noexcept {
  RigidTransform transform;
  // REP-103 optical (right, down, forward) to base_link (forward, left, up),
  // plus the current rig pose and logical-left half-baseline.
  transform.matrix = {
      0.0, 0.0, 1.0, 0.760,
      -1.0, 0.0, 0.0, 0.050,
      0.0, -1.0, 0.0, 0.180,
      0.0, 0.0, 0.0, 1.0,
  };
  return transform;
}

class NativeOutputPublisher::Impl {
 public:
  explicit Impl(NativeOutputConfig config)
      : config_(std::move(config)),
        obstacle_cloud_(config_.obstacle_cloud),
        result_ring_(config_.ring) {}

  NativeOutputStats publish(const NativeOutputFrameView& frame) {
    if (frame.capture_realtime_ns <= 0) {
      throw std::invalid_argument("native output capture timestamp is invalid");
    }
    if (frame.disparity.data == nullptr ||
        frame.disparity.width != ipc::kImageWidth ||
        frame.disparity.height != ipc::kImageHeight ||
        frame.disparity.stride_elements != frame.disparity.width) {
      throw std::invalid_argument(
          "native output requires contiguous 728x544 disparity");
    }

    const auto started = SteadyClock::now();
    const auto cloud = obstacle_cloud_.generate(
        frame.disparity, config_.stereo_camera,
        config_.optical_to_obstacle_frame);
    const auto cloud_ready = SteadyClock::now();
    ResultSnapshotView snapshot;
    snapshot.capture_realtime_ns = frame.capture_realtime_ns;
    snapshot.ready_realtime_ns =
        std::max(realtime_now_ns(), frame.capture_realtime_ns);
    snapshot.health = frame.health;
    snapshot.disparity = std::span<const float>(
        frame.disparity.data,
        static_cast<std::size_t>(frame.disparity.width) *
            frame.disparity.height);
    snapshot.left_rectified_mono = frame.left_rectified_mono;
    snapshot.left_rectified_jpeg = frame.left_rectified_jpeg;
    snapshot.obstacle_points = cloud.points;
    snapshot.obstacle_stats = cloud.stats;
    const auto sequence = result_ring_.publish(snapshot);
    const auto finished = SteadyClock::now();

    NativeOutputStats stats;
    stats.sequence = sequence;
    stats.obstacle_point_count =
        static_cast<std::uint32_t>(cloud.points.size());
    stats.obstacle_cloud_ms = elapsed_ms(started, cloud_ready);
    stats.result_ring_publish_ms = elapsed_ms(cloud_ready, finished);
    stats.total_ms = elapsed_ms(started, finished);
    stats.obstacle_cloud = cloud.stats;
    return stats;
  }

 private:
  NativeOutputConfig config_;
  CompactObstacleCloudGenerator obstacle_cloud_;
  ResultRingWriter result_ring_;
};

NativeOutputPublisher::NativeOutputPublisher(NativeOutputConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
NativeOutputPublisher::~NativeOutputPublisher() = default;
NativeOutputPublisher::NativeOutputPublisher(NativeOutputPublisher&&) noexcept =
    default;
NativeOutputPublisher& NativeOutputPublisher::operator=(
    NativeOutputPublisher&&) noexcept = default;
NativeOutputStats NativeOutputPublisher::publish(
    const NativeOutputFrameView& frame) {
  return impl_->publish(frame);
}

}  // namespace depthd
