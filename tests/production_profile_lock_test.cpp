#include "depthd/contract.hpp"
#include "depthd/ipc/result_ring_layout.hpp"
#include "depthd/native_output.hpp"
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
      sgbm.working_min_disparity != 0 ||
      sgbm.accepted_min_disparity_px != 0.0F ||
      sgbm.accepted_max_disparity_px != 64.0F ||
      sgbm.working_disparities != 64 || sgbm.block_size != 5 ||
      sgbm.sgbm_mode != "3way" ||
      sgbm.uniqueness_ratio != 1 || sgbm.speckle_window_size != 30 ||
      sgbm.speckle_range != 2 || sgbm.disparity_12_max_diff != 1 ||
      sgbm.pre_filter_cap != 31 || sgbm.opencv_threads != 1) {
    return 1;
  }
  if (depthd::kImageWidth != 728 || depthd::kImageHeight != 544 ||
      depthd::kRectifiedFocalLengthPx != 493.542046188F ||
      depthd::kBaselineM != 0.100F ||
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
  const auto optical_to_base =
      depthd::make_nuvo_left_optical_to_base_transform();
  if (optical_to_base.matrix[0] != 0.0 ||
      optical_to_base.matrix[2] != 1.0 ||
      optical_to_base.matrix[3] != 0.760 ||
      optical_to_base.matrix[4] != -1.0 ||
      optical_to_base.matrix[7] != 0.050 ||
      optical_to_base.matrix[9] != -1.0 ||
      optical_to_base.matrix[11] != 0.180) {
    return 7;
  }

  const std::string config =
      read_file(std::string(DEPTHD_SOURCE_DIR) +
                "/config/nuvo_pi5_depth_100mm.yaml");
  if (!contains_all(
          config,
          {"left_camera_index: 0", "right_camera_index: 1", "width: 728",
           "height: 544", "stream_profile: isp_luma_rgb_left",
           "main_pixel_format: YUV420", "depth_plane: Y",
           "left_color_stream: lores", "left_color_pixel_format: RGB888",
           "right_color_stream_enabled: false", "colour_space: sYCC",
           "orientation_degrees: 180", "target_hz: 10", "buffer_count: 4",
           "line: 17", "pulse_width_us: 8000", "pair_tolerance_us: 500",
           "auto_exposure:", "enabled: false",
           "max_shutter_us: 8000", "max_analogue_gain: 3.0",
           "backend: cpu_opencv", "fallback_backend: egl_gl_dmabuf",
           "left_calibration: /var/lib/nuvo-depthd/current/config/calibration/nuvo_depth_100mm/front_left.yaml",
           "right_calibration: /var/lib/nuvo-depthd/current/config/calibration/nuvo_depth_100mm/front_right.yaml",
           "depth_preprocessing: isp_luma_before_rectification",
           "backend: cpu_sgbm", "max_disparity: 128",
           "working_width: 728", "working_height: 544",
           "working_disparities: 128", "block_size: 5", "mode: sgbm_3way",
           "uniqueness_ratio: 0", "median_filter_size: 0", "speckle_window_size: 30",
           "speckle_range: 2", "opencv_threads: 2",
           "disparity_hz: 10", "metric_depth_enabled: false",
           "compressed_image_hz: 2", "jpeg_quality: 75",
           "result_ring_path: /run/depthd/results.ring",
           "result_ring_slots: 4", "obstacle_cloud_enabled: false",
           "obstacle_cloud_topic: /camera/front/obstacles",
           "role: standalone_depth_camera", "profile_id: balanced_728x544_10",
           "baseline_m: 0.100",
           "calibration_status: provisional_recalibration_required"})) {
    return 3;
  }

  const std::string encoding =
      read_file(std::string(DEPTHD_SOURCE_DIR) + "/config/encoding.json");
  if (!contains_all(encoding,
                    {"\"schema_version\":1", "\"mode\":\"raw\"",
                     "\"codec\":\"h264\"",
                     "\"source\":\"left_rectified\"", "\"width\":0",
                     "\"height\":0", "\"cadence_hz\":10",
                     "\"bitrate_bps\":4000000", "\"threads\":1",
                     "\"low_latency\":true",
                     "\"preset\":\"veryfast\""})) {
    return 9;
  }

  const std::string left_calibration =
      read_file(std::string(DEPTHD_SOURCE_DIR) +
                "/config/calibration/nuvo_depth_100mm/front_left.yaml");
  const std::string right_calibration =
      read_file(std::string(DEPTHD_SOURCE_DIR) +
                "/config/calibration/nuvo_depth_100mm/front_right.yaml");
  if (!contains_all(left_calibration,
                    {"calibration_status: provisional_recalibration_required",
                     "baseline_m: 0.100",
                     "data: [493.542046188, 0, 348.240810394, 0,"}) ||
      !contains_all(right_calibration,
                    {"calibration_status: provisional_recalibration_required",
                     "baseline_m: 0.100",
                     "data: [493.542046188, 0, 348.240810394, -49.3542046188,"})) {
    return 8;
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
           "BASELINE_M = 0.100"})) {
    return 4;
  }
  return 0;
}
