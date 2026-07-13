#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "depthd/ipc/result_ring_layout.hpp"
#include "depthd/obstacle_cloud.hpp"

namespace depthd {

struct ResultRingConfig {
  std::string path{"/run/depthd/results.ring"};
  std::uint32_t file_mode{0644};
  std::uint64_t calibration_id{1};
  ipc::CameraModel left_camera{};
  ipc::CameraModel right_camera{};
  std::string left_optical_frame{"camera_front_left_optical"};
  std::string right_optical_frame{"camera_front_right_optical"};
  std::string obstacle_frame{"base_link"};
};

struct ResultSnapshotView {
  std::int64_t capture_realtime_ns{};
  std::int64_t ready_realtime_ns{};
  ipc::HealthState health{ipc::HealthState::kStarting};
  std::span<const float> disparity{};
  std::span<const std::uint8_t> left_rectified_mono{};
  std::span<const std::uint8_t> left_rectified_jpeg{};
  std::span<const Point3f> obstacle_points{};
  CompactObstacleCloudStats obstacle_stats{};
};

struct ResultSnapshot {
  ipc::ResultSlotHeader header{};
  std::vector<float> disparity{};
  std::vector<std::uint8_t> left_rectified_mono{};
  std::vector<std::uint8_t> left_rectified_jpeg{};
  std::vector<Point3f> obstacle_points{};
};

enum class ResultRingReadStatus {
  kOk,
  kMissing,
  kSchemaMismatch,
  kTorn,
  kStale,
  kRestarted,
  kUnavailable,
};

struct ResultRingReadResult {
  ResultRingReadStatus status{ResultRingReadStatus::kUnavailable};
  std::string message{};
  std::uint64_t observed_sequence{};
  std::uint64_t dropped_sequences{};
  std::uint64_t torn_reads{};
  std::uint64_t schema_mismatches{};
};

class ResultRingWriter {
 public:
  explicit ResultRingWriter(ResultRingConfig config = {});
  ~ResultRingWriter();

  ResultRingWriter(ResultRingWriter&&) noexcept;
  ResultRingWriter& operator=(ResultRingWriter&&) noexcept;
  ResultRingWriter(const ResultRingWriter&) = delete;
  ResultRingWriter& operator=(const ResultRingWriter&) = delete;

  [[nodiscard]] std::uint64_t instance_id() const noexcept;
  [[nodiscard]] std::uint64_t last_sequence() const noexcept;
  [[nodiscard]] std::uint64_t publish(const ResultSnapshotView& snapshot);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class ResultRingReader {
 public:
  explicit ResultRingReader(std::string path = "/run/depthd/results.ring");
  ~ResultRingReader();

  ResultRingReader(ResultRingReader&&) noexcept;
  ResultRingReader& operator=(ResultRingReader&&) noexcept;
  ResultRingReader(const ResultRingReader&) = delete;
  ResultRingReader& operator=(const ResultRingReader&) = delete;

  [[nodiscard]] ResultRingReadResult read_latest(ResultSnapshot& snapshot);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] ipc::CameraModel make_rectified_left_camera_model() noexcept;
[[nodiscard]] ipc::CameraModel make_rectified_right_camera_model() noexcept;
[[nodiscard]] const char* result_ring_read_status_name(
    ResultRingReadStatus status) noexcept;

}  // namespace depthd
