#include "depthd/capture.hpp"
#include "depthd/config.hpp"
#include "depthd/gateway.hpp"
#include "depthd/ipc_v2.hpp"
#include "depthd/products.hpp"
#include "depthd/wire_protocol.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

int fail(const int code, const std::string_view message) {
  std::cerr << "architecture_coherence_test: " << message << '\n';
  return code;
}

depthd::CapturedFrame frame(const depthd::CameraRole role,
                            const std::string_view camera_id,
                            const std::uint64_t epoch,
                            std::atomic<int>& releases) {
  depthd::CapturedFrame result;
  result.role = role;
  result.camera_stable_id = camera_id;
  result.capture_epoch = epoch;
  result.stream_sequence = 1U;
  result.sensor_timestamp_ns =
      role == depthd::CameraRole::kLogicalLeft ? 9'000'000'000LL
                                               : 9'000'020'000LL;
  result.sensor_clock = depthd::CaptureClock::kBoottime;
  result.grid = depthd::make_reference_grid(728U, 544U);
  result.pixel_format = "YUV420";
  result.planes.push_back({7, 0U, 728U * 544U, 728U});
  result.lease = depthd::CaptureBufferLease(
      epoch, role == depthd::CameraRole::kLogicalLeft ? 1U : 2U,
      [&releases] { ++releases; });
  return result;
}

}  // namespace

int main() {
  const auto loaded = depthd::config::load(
      std::filesystem::path(DEPTHD_SOURCE_DIR) /
      "config/nuvo_pi5_depth_100mm.yaml");
  const auto& config = loaded.effective;
  if (loaded.configuration_sha256 !=
      "1a86bd8b9bd9dcc39f17623e7088add9f1c9f088dd63af7414d44f5cc388a025") {
    return fail(1, "Step 1 effective configuration identity changed");
  }
  if (config.capture.width != config.stereo.output_width ||
      config.capture.height != config.stereo.output_height ||
      std::abs(config.rig.baseline_m - config.stereo.baseline_m) > 1e-12) {
    return fail(2, "capture, rig, and stereo geometry disagree");
  }
  if (config.rig.calibration_status !=
          depthd::config::CalibrationStatus::kProvisionalRecalibrationRequired ||
      config.outputs.metric_depth_enabled) {
    return fail(3, "provisional calibration was promoted to metric authority");
  }

  depthd::CaptureOpenOptions capture;
  capture.left_camera_index = config.capture.left_camera_index;
  capture.right_camera_index = config.capture.right_camera_index;
  capture.width = config.capture.width;
  capture.height = config.capture.height;
  capture.pixel_format = config.capture.main_pixel_format;
  capture.orientation_degrees = config.capture.orientation_degrees;
  capture.buffer_count = config.capture.buffer_count;
  capture.target_hz = config.capture.target_hz;
  capture.pair_tolerance_ns =
      static_cast<std::uint64_t>(config.capture.hardware_trigger.pair_tolerance_us) *
      1'000U;
  capture.hardware_trigger_enabled =
      config.capture.hardware_trigger.enabled;
  capture.trigger_driver_mode = config.capture.hardware_trigger.driver_mode;
  capture.gpiochip = config.capture.hardware_trigger.gpiochip;
  capture.gpio_line = config.capture.hardware_trigger.line;
  capture.trigger_idle_high = config.capture.hardware_trigger.idle_level == "high";
  capture.trigger_pulse_width_us =
      config.capture.hardware_trigger.pulse_width_us;
  capture.startup_priming_pulses =
      config.capture.hardware_trigger.startup_priming_pulses;
  capture.exposure_time_us = config.capture.exposure_time_us;
  capture.analogue_gain = config.capture.analogue_gain;
  try {
    depthd::validate(capture);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return fail(4, "Step 1 configuration cannot construct Step 3 capture");
  }

  const depthd::CameraBinding left{
      depthd::CameraRole::kLogicalLeft,
      {"platform/csi0/imx296", "imx296", "csi", capture.left_camera_index}};
  const depthd::CameraBinding right{
      depthd::CameraRole::kLogicalRight,
      {"platform/csi1/imx296", "imx296", "csi", capture.right_camera_index}};
  depthd::StereoPairer pairer(
      {capture.pair_tolerance_ns, capture.buffer_count});
  const auto epoch = pairer.begin_epoch(left, right);
  std::atomic<int> releases{};
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch, releases));
  auto pair = pairer.submit(frame(depthd::CameraRole::kLogicalRight,
                                  right.camera.stable_id, epoch, releases));
  if (!pair) {
    return fail(5, "Step 3 capture pair did not associate");
  }

  std::array<std::uint8_t, 16> device_uuid{};
  std::array<std::uint8_t, 16> run_uuid{};
  device_uuid[0] = 1U;
  run_uuid[0] = 2U;
  const auto frame_identity =
      depthd::make_frame_set_identity(*pair, device_uuid, run_uuid, 1U);
  const auto image_grid =
      depthd::make_reference_grid(config.capture.width, config.capture.height);
  const auto native_grid = depthd::make_resampled_grid(
      image_grid, config.stereo.working_width, config.stereo.working_height);
  const double focal = config.stereo.rectified_focal_length_px;
  const double cx = static_cast<double>(config.capture.width) / 2.0;
  const double cy = static_cast<double>(config.capture.height) / 2.0;
  const std::array<double, 12> left_projection{
      focal, 0.0, cx, 0.0, 0.0, focal, cy, 0.0, 0.0, 0.0, 1.0, 0.0};
  auto right_projection = left_projection;
  right_projection[3] = -focal * config.stereo.baseline_m;
  const auto calibration_identity = depthd::calibration_identity_from_sha256(
      "0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef");
  const auto image_calibration = depthd::make_rectified_calibration(
      calibration_identity,
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired,
      image_grid, left_projection, right_projection);
  const auto matching_calibration =
      depthd::resample_rectified_calibration(image_calibration, native_grid);

  depthd::ProductSetMetadata products;
  products.frame = frame_identity;
  products.calibration = calibration_identity;
  products.calibration_authority =
      depthd::CalibrationAuthority::kProvisionalRecalibrationRequired;
  products.left_rectified = {
      image_grid, {0U, 0U, image_grid.width, image_grid.height},
      depthd::CameraRole::kLogicalLeft, depthd::ImageProductFormat::kGray8};
  products.right_rectified = {
      image_grid, {0U, 0U, image_grid.width, image_grid.height},
      depthd::CameraRole::kLogicalRight, depthd::ImageProductFormat::kGray8};
  products.disparity.grid = native_grid;
  products.disparity.valid_roi = {
      config.stereo.working_disparities + config.stereo.block_size / 2U, 0U,
      native_grid.width - config.stereo.working_disparities -
          config.stereo.block_size / 2U,
      native_grid.height};
  products.disparity.accepted_min_disparity_px =
      static_cast<float>(config.stereo.min_disparity) /
      static_cast<float>(native_grid.x_scale_to_reference);
  products.disparity.accepted_max_disparity_px =
      static_cast<float>(config.stereo.max_disparity) /
      static_cast<float>(native_grid.x_scale_to_reference);
  products.disparity.algorithmic_quantization_px = 1.0F / 16.0F;
  products.disparity.confidence_model =
      depthd::ConfidenceModel::kBinarySgbmSupportV1;
  products.depth.grid = native_grid;
  products.depth.minimum_depth_m = 0.3F;
  products.depth.maximum_depth_m = 20.0F;
  try {
    depthd::validate(products, image_calibration, matching_calibration);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return fail(6, "Steps 1-3 do not form one valid product contract");
  }

  try {
    const auto pool = depthd::ipc_v2::make_pool_configuration(products, 6U);
    depthd::ipc_v2::validate(pool);
    const auto image_pixels = static_cast<std::uint64_t>(image_grid.width) *
                              image_grid.height;
    const auto depth_pixels = static_cast<std::uint64_t>(native_grid.width) *
                              native_grid.height;
    const auto payload_bytes = 2U * image_pixels + 10U * depth_pixels;
    if (payload_bytes != 4'752'384U) {
      return fail(8, "Steps 2 and 5 disagree on full-resolution product bytes");
    }
    depthd::wire::Header header;
    header.kind = depthd::wire::MessageKind::kFrameSet;
    header.metadata_bytes = 4096U;
    header.payload_bytes = payload_bytes;
    header.stream_sequence = 1U;
    const auto encoded = depthd::wire::encode_header(header);
    if (depthd::wire::decode_header(encoded).payload_bytes != payload_bytes) {
      return fail(9, "Steps 5 and 6 disagree on framed product bytes");
    }
    depthd::gateway::AdmissionConfiguration transports;
    transports.preference =
        depthd::gateway::TransportPreference::kEthernetPreferred;
    transports.ethernet = {true, true, 80U * 1000U * 1000U,
                           64U * 1024U * 1024U, 4U};
    transports.usb_ethernet = {true, true, 25U * 1000U * 1000U,
                              25U * 1024U * 1024U, 2U};
    depthd::gateway::AdmissionController gateway(transports);
    const auto admission = gateway.admit(
        {depthd::ipc_v2::kAllProducts, payload_bytes, 4096U,
         config.outputs.disparity_hz, false, false});
    if (!admission.lease ||
        admission.lease->transport() !=
            depthd::gateway::TransportKind::kEthernet) {
      return fail(10, "Step 6 cannot admit the Step 1 full-resolution preset");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return fail(11, "Steps 1-6 do not form one bounded transport contract");
  }

  pair.reset();
  pairer.end_epoch();
  if (releases.load() != 2) {
    return fail(7, "coherent product path did not release both camera buffers");
  }
  return 0;
}
