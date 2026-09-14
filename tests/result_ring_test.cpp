#include "depthd/result_ring.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
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

template <typename Callable>
void expect_invalid(Callable&& callable, const std::string& message) {
  try {
    callable();
    expect(false, message);
  } catch (const std::invalid_argument&) {
  }
}

#if defined(__linux__) || defined(__APPLE__)
std::filesystem::path temporary_ring_path() {
  const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("depthd-result-ring-" + std::to_string(token) + ".ring");
}

depthd::ResultSnapshotView view(const std::vector<float>& disparity,
                                const std::vector<std::uint8_t>& mono,
                                const std::vector<std::uint8_t>& jpeg,
                                const std::vector<depthd::Point3f>& points) {
  depthd::ResultSnapshotView snapshot;
  snapshot.capture_realtime_ns = 1'780'000'000'000'000'000LL;
  snapshot.ready_realtime_ns = snapshot.capture_realtime_ns + 25'000'000LL;
  snapshot.health = depthd::ipc::HealthState::kOk;
  snapshot.disparity = disparity;
  snapshot.left_rectified_mono = mono;
  snapshot.left_rectified_jpeg = jpeg;
  snapshot.obstacle_points = points;
  snapshot.obstacle_stats.sampled_pixels = 48600;
  snapshot.obstacle_stats.accepted_raw_points = 7588;
  snapshot.obstacle_stats.accumulated_voxels = 2251;
  snapshot.obstacle_stats.supported_voxels = 384;
  snapshot.obstacle_stats.accepted_clusters = 5;
  return snapshot;
}
#endif

}  // namespace

int main() {
  expect(depthd::ipc::kResultRingFileBytes == 11620352ULL,
         "ring file size is locked");
  expect(depthd::ipc::kResultRingSlotCount == 4,
         "four newest-result slots are locked");
  expect(depthd::ipc::kDisparityCapacityBytes == 1584128U,
         "disparity capacity matches 728x544 float32");
  const auto left = depthd::make_rectified_left_camera_model();
  const auto right = depthd::make_rectified_right_camera_model();
  expect(left.p[3] == 0.0, "logical left projection has no baseline offset");
  expect(std::abs(right.p[3] + 49.3542046188) < 1e-9,
         "logical right projection encodes the 100 mm baseline");

#if defined(__linux__) || defined(__APPLE__)
  const auto path = temporary_ring_path();
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  {
    depthd::ResultRingConfig config;
    config.path = path.string();
    depthd::ResultRingWriter writer(config);
    depthd::ResultRingReader reader(path.string());
    depthd::ResultSnapshot output;
    auto empty = reader.read_latest(output);
    expect(empty.status == depthd::ResultRingReadStatus::kUnavailable,
           "new ring has no complete result");

    std::vector<float> disparity(
        static_cast<std::size_t>(depthd::ipc::kImageWidth) *
        depthd::ipc::kImageHeight,
        12.5F);
    disparity.front() = std::numeric_limits<float>::quiet_NaN();
    std::vector<std::uint8_t> mono(depthd::ipc::kMonoCapacityBytes, 73U);
    const std::vector<std::uint8_t> jpeg{0xff, 0xd8, 1, 2, 3, 0xff, 0xd9};
    const std::vector<depthd::Point3f> points{
        {1.0F, 0.1F, 0.2F}, {2.0F, -0.2F, 0.3F}};
    const auto sequence = writer.publish(view(disparity, mono, jpeg, points));
    expect(sequence == 1U, "first published sequence is one");
    const auto result = reader.read_latest(output);
    expect(result.status == depthd::ResultRingReadStatus::kOk,
           "reader accepts complete result");
    expect(result.observed_sequence == 1U, "reader reports sequence");
    expect(output.disparity.size() == disparity.size(),
           "reader copies complete disparity");
    expect(std::isnan(output.disparity.front()), "NaN disparity is preserved");
    expect(output.disparity[1] == 12.5F, "finite disparity is preserved");
    expect(output.left_rectified_mono == mono, "mono payload is preserved");
    expect(output.left_rectified_jpeg == jpeg, "JPEG payload is preserved");
    expect(output.obstacle_points.size() == points.size(),
           "cloud point count is preserved");
    expect(output.obstacle_points[1].y == points[1].y,
           "cloud coordinates are preserved");
    expect(output.header.left_camera.p[3] == 0.0,
           "ring contains logical left CameraInfo");
    expect(output.header.right_camera.p[3] < 0.0,
           "ring contains logical right CameraInfo");
    expect(output.header.delta_d == 0.0625F,
           "ring publishes actual restored disparity increment");
    expect(output.header.valid_window_x_offset == 128U &&
               output.header.valid_window_width == 600U,
           "ring publishes locked valid window");
    expect(output.header.sampled_pixels == 48600U,
           "cloud diagnostics are preserved");
    const auto stale = reader.read_latest(output);
    expect(stale.status == depthd::ResultRingReadStatus::kStale,
           "same sequence is not published twice");

    auto bad = view(disparity, mono, jpeg, points);
    bad.disparity = std::span<const float>(disparity.data(), 2);
    expect_invalid([&] { (void)writer.publish(bad); },
                   "wrong disparity geometry is rejected");
    bad = view(disparity, mono, jpeg, points);
    bad.capture_realtime_ns = bad.ready_realtime_ns + 1;
    expect_invalid([&] { (void)writer.publish(bad); },
                   "backwards timestamps are rejected");
  }

  {
    depthd::ResultRingReader reader(path.string());
    depthd::ResultSnapshot output;
    const auto first = reader.read_latest(output);
    expect(first.status == depthd::ResultRingReadStatus::kOk,
           "fresh reader accepts prior complete result");
    const auto old_instance = output.header.instance_id;
    depthd::ResultRingConfig config;
    config.path = path.string();
    depthd::ResultRingWriter writer(config);
    std::vector<float> disparity(
        static_cast<std::size_t>(depthd::ipc::kImageWidth) *
        depthd::ipc::kImageHeight,
        5.0F);
    const std::vector<std::uint8_t> empty;
    const std::vector<depthd::Point3f> points;
    (void)writer.publish(view(disparity, empty, empty, points));
    const auto restarted = reader.read_latest(output);
    expect(restarted.status == depthd::ResultRingReadStatus::kRestarted,
           "reader detects daemon writer restart");
    expect(output.header.instance_id != old_instance,
           "restart has a new instance id");
  }
  std::filesystem::remove(path, ignored);
#endif

  return failures == 0 ? 0 : 1;
}
