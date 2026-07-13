#include "depthd/contract.hpp"
#include "depthd/ipc/result_ring_layout.hpp"
#include "depthd/obstacle_cloud.hpp"
#include "depthd/stereo_backend.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

std::string read_file(const std::string& path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

bool contains_all(const std::string& text,
                  const std::initializer_list<std::string_view> values) {
  for (const auto value : values) {
    if (text.find(value) == std::string::npos) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  const depthd::CpuSgbmConfig sgbm;
  if (sgbm.input_width != 728 || sgbm.input_height != 544 ||
      sgbm.working_width != 364 || sgbm.working_height != 272 ||
      sgbm.min_disparity != 0 ||
      sgbm.full_resolution_disparities != 128 ||
      sgbm.working_disparities != 64 || sgbm.block_size != 5 ||
      sgbm.uniqueness_ratio != 1 || sgbm.speckle_window_size != 30 ||
      sgbm.speckle_range != 2 || sgbm.disparity_12_max_diff != 1 ||
      sgbm.pre_filter_cap != 31 || sgbm.opencv_threads != 1) {
    return 1;
  }
  if (depthd::kImageWidth != 728 || depthd::kImageHeight != 544 ||
      depthd::kRectifiedFocalLengthPx != 493.542046188F ||
      depthd::kBaselineM != 0.161458780F ||
      depthd::kLeftOpticalFrame != "camera_front_left_optical" ||
      depthd::kRightOpticalFrame != "camera_front_right_optical") {
    return 2;
  }
  const depthd::CompactObstacleCloudConfig cloud;
  if (cloud.roi_y_min != 96 || cloud.roi_y_max != 420 ||
      cloud.row_step != 2 || cloud.col_step != 2 ||
      cloud.depth_min_m != 0.75 || cloud.depth_max_m != 6.0 ||
      cloud.min_height_m != 0.18 || cloud.max_height_m != 1.20 ||
      cloud.angle_min_rad != -0.70 || cloud.angle_max_rad != 0.70 ||
      cloud.voxel_leaf_size_m != 0.05 ||
      cloud.min_points_per_voxel != 5 ||
      cloud.min_cluster_voxels != 12 ||
      cloud.min_cluster_raw_points != 60 || cloud.max_voxels != 32768) {
    return 5;
  }
  if (depthd::ipc::kResultRingSchemaVersion != 1 ||
      depthd::ipc::kResultRingSlotCount != 4 ||
      depthd::ipc::kResultRingFileBytes != 11620352ULL ||
      depthd::ipc::kImageWidth != 728 ||
      depthd::ipc::kImageHeight != 544 ||
      depthd::ipc::kDisparityStrideBytes != 2912 ||
      depthd::ipc::kDisparityCapacityBytes != 1584128 ||
      depthd::ipc::kObstaclePointCapacity != 32768) {
    return 6;
  }

  const std::string config =
      read_file(std::string(DEPTHD_SOURCE_DIR) +
                "/config/nuvo_pi5_hailo8l.yaml");
  if (!contains_all(
          config,
          {"left_camera_index: 1", "right_camera_index: 0", "width: 728",
           "height: 544", "pixel_format: RGB888",
           "orientation_degrees: 180", "target_hz: 10", "buffer_count: 4",
           "line: 17", "pulse_width_us: 8000", "pair_tolerance_us: 500",
           "max_shutter_us: 8000", "max_analogue_gain: 3.0",
           "backend: cpu_opencv", "fallback_backend: cpu_opencv",
           "left_calibration: calibration/nuvo_reference/front_right.yaml",
           "right_calibration: calibration/nuvo_reference/front_left.yaml",
           "depth_preprocessing: grayscale_before_rectification",
           "backend: cpu_sgbm", "max_disparity: 128",
           "working_width: 364", "working_height: 272",
           "working_disparities: 64", "block_size: 5", "mode: sgbm_3way",
           "uniqueness_ratio: 1", "speckle_window_size: 30",
           "speckle_range: 2", "opencv_threads: 1",
           "disparity_hz: 10", "metric_depth_enabled: false",
           "compressed_image_hz: 2", "jpeg_quality: 75",
           "result_ring_path: /run/depthd/results.ring",
           "result_ring_slots: 4", "obstacle_cloud_enabled: true",
           "obstacle_cloud_topic: /camera/front/obstacles"})) {
    return 3;
  }

  const std::string profiler =
      read_file(std::string(DEPTHD_SOURCE_DIR) +
                "/tools/pi/profile_cpu_pipeline.py");
  if (!contains_all(
          profiler,
          {"WIDTH = 728", "HEIGHT = 544", "WORKING_WIDTH = 364",
           "WORKING_HEIGHT = 272", "WORKING_DISPARITIES = 64",
           "FULL_DISPARITY_LIMIT = 128.0",
           "FOCAL_LENGTH_PX = 493.542046188",
           "BASELINE_M = 0.161458780"})) {
    return 4;
  }
  return 0;
}
