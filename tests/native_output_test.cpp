#include "depthd/native_output.hpp"
#include "depthd/native_output_c.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
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

#if defined(__linux__) || defined(__APPLE__)
std::filesystem::path temporary_ring_path(const std::string& suffix) {
  const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("depthd-native-output-" + std::to_string(token) + suffix);
}

std::vector<float> obstacle_plane() {
  const float disparity =
      493.542046188F * 0.100F / 2.0F;
  return std::vector<float>(
      static_cast<std::size_t>(depthd::ipc::kImageWidth) *
          depthd::ipc::kImageHeight,
      disparity);
}
#endif

}  // namespace

int main() {
  const auto transform = depthd::make_nuvo_left_optical_to_base_transform();
  expect(transform.matrix[3] == 0.760 && transform.matrix[7] == 0.050 &&
             transform.matrix[11] == 0.180,
         "native cloud transform locks the current Nuvo rig pose");

#if defined(__linux__) || defined(__APPLE__)
  std::error_code ignored;
  const auto path = temporary_ring_path(".ring");
  std::filesystem::remove(path, ignored);
  auto disparity = obstacle_plane();
  std::vector<std::uint8_t> mono(depthd::ipc::kMonoCapacityBytes, 80U);
  const std::vector<std::uint8_t> jpeg{0xff, 0xd8, 7, 8, 0xff, 0xd9};
  {
    depthd::NativeOutputConfig config;
    config.ring.path = path.string();
    config.optical_to_obstacle_frame = transform;
    depthd::NativeOutputPublisher publisher(config);
    depthd::NativeOutputFrameView frame;
    frame.capture_realtime_ns = 1'780'000'000'000'000'000LL;
    frame.health = depthd::ipc::HealthState::kOk;
    frame.disparity = {
        disparity.data(), depthd::ipc::kImageWidth,
        depthd::ipc::kImageHeight, depthd::ipc::kImageWidth,
        128, 0, 600, 544, 0.0F, 128.0F};
    frame.left_rectified_mono = mono;
    frame.left_rectified_jpeg = jpeg;
    const auto stats = publisher.publish(frame);
    expect(stats.sequence == 1U, "native output publishes first sequence");
    expect(stats.obstacle_point_count > 0U,
           "native output produces compact obstacle centroids");
    expect(stats.obstacle_cloud.voxel_capacity_overflow == 0U,
           "synthetic cloud stays within fixed capacity");

    depthd::ResultRingReader reader(path.string());
    depthd::ResultSnapshot snapshot;
    const auto result = reader.read_latest(snapshot);
    expect(result.status == depthd::ResultRingReadStatus::kOk,
           "native output snapshot is readable");
    expect(snapshot.obstacle_points.size() == stats.obstacle_point_count,
           "native output writes its compact cloud");
    expect(snapshot.left_rectified_mono == mono,
           "native output writes optional mono unchanged");
    expect(snapshot.left_rectified_jpeg == jpeg,
           "native output writes scheduled JPEG unchanged");
    expect(snapshot.header.capture_realtime_ns == frame.capture_realtime_ns,
           "native output preserves capture timestamp");
    expect(snapshot.header.ready_realtime_ns >= frame.capture_realtime_ns,
           "native output records completion timestamp");
    for (const auto& point : snapshot.obstacle_points) {
      expect(point.x >= 0.75F && point.x <= 6.0F,
             "cloud point remains in navigation range");
      expect(point.z >= 0.18F && point.z <= 1.20F,
             "cloud point remains in navigation height band");
    }
  }
  std::filesystem::remove(path, ignored);

  const auto c_path = temporary_ring_path("-c.ring");
  std::filesystem::remove(c_path, ignored);
  depthd_native_output_config config{};
  depthd_native_output_default_config(&config);
  const auto c_path_string = c_path.string();
  config.result_ring_path = c_path_string.c_str();
  char error[256]{};
  auto* handle = depthd_native_output_create(&config, error, sizeof(error));
  expect(handle != nullptr, std::string("C API creates publisher: ") + error);
  if (handle != nullptr) {
    depthd_native_output_frame frame{};
    frame.struct_size = sizeof(frame);
    frame.capture_realtime_ns = 1'780'000'000'100'000'000LL;
    frame.health = 1;
    frame.disparity = disparity.data();
    frame.width = depthd::ipc::kImageWidth;
    frame.height = depthd::ipc::kImageHeight;
    frame.disparity_stride_elements = depthd::ipc::kImageWidth;
    depthd_native_output_stats stats{};
    stats.struct_size = sizeof(stats);
    const int code = depthd_native_output_publish(
        handle, &frame, &stats, error, sizeof(error));
    expect(code == 0, std::string("C API publishes snapshot: ") + error);
    expect(stats.sequence == 1U && stats.obstacle_point_count > 0U,
           "C API returns native output statistics");
    depthd_native_output_destroy(handle);
  }
  std::filesystem::remove(c_path, ignored);
#endif

  return failures == 0 ? 0 : 1;
}
