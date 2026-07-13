#include "depthd/result_ring.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  const std::string path =
      argc > 1 ? argv[1] : "/run/depthd/results.ring";
  depthd::ResultRingReader reader(path);
  depthd::ResultSnapshot snapshot;
  const auto result = reader.read_latest(snapshot);
  std::cout << "status="
            << depthd::result_ring_read_status_name(result.status) << '\n'
            << "message=" << result.message << '\n'
            << "sequence=" << result.observed_sequence << '\n'
            << "dropped_sequences=" << result.dropped_sequences << '\n'
            << "torn_reads=" << result.torn_reads << '\n'
            << "schema_mismatches=" << result.schema_mismatches << '\n';
  if (result.status != depthd::ResultRingReadStatus::kOk &&
      result.status != depthd::ResultRingReadStatus::kRestarted) {
    return 1;
  }
  std::size_t valid_disparities = 0;
  for (const float value : snapshot.disparity) {
    valid_disparities += std::isfinite(value) ? 1U : 0U;
  }
  std::cout << "instance_id=" << snapshot.header.instance_id << '\n'
            << "capture_realtime_ns="
            << snapshot.header.capture_realtime_ns << '\n'
            << "ready_realtime_ns=" << snapshot.header.ready_realtime_ns
            << '\n'
            << "disparity_elements=" << snapshot.disparity.size() << '\n'
            << "valid_disparities=" << valid_disparities << '\n'
            << "mono_bytes=" << snapshot.left_rectified_mono.size() << '\n'
            << "jpeg_bytes=" << snapshot.left_rectified_jpeg.size() << '\n'
            << "obstacle_points=" << snapshot.obstacle_points.size() << '\n'
            << "sampled_pixels=" << snapshot.header.sampled_pixels << '\n'
            << "accepted_raw_points="
            << snapshot.header.accepted_raw_points << '\n'
            << "supported_voxels=" << snapshot.header.supported_voxels
            << '\n'
            << "accepted_clusters=" << snapshot.header.accepted_clusters
            << '\n'
            << "voxel_capacity_overflow="
            << snapshot.header.voxel_capacity_overflow << '\n';
  return 0;
}
