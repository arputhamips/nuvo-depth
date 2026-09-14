#include "depthd/frame_wire.hpp"

#include <cassert>
#include <cstring>
#include <string>

#include "depthd/runtime.hpp"

namespace {

depthd::FrameSetStorage frame() {
  depthd::FrameSetStorage result;
  result.metadata.frame.device_uuid[0] = 1U;
  result.metadata.frame.run_uuid[0] = 2U;
  result.metadata.frame.capture_epoch = 1U;
  result.metadata.frame.capture_sequence = 2U;
  result.metadata.frame.configuration_epoch = 3U;
  result.metadata.frame.capture_time_ns = 4;
  result.metadata.frame.capture_clock = depthd::CaptureClock::kBoottime;
  result.metadata.calibration.sha256[0] = 3U;
  const auto grid = depthd::make_reference_grid(2U, 1U);
  const depthd::RegionU32 roi{0U, 0U, 2U, 1U};
  result.metadata.left_rectified = {
      grid, roi, depthd::CameraRole::kLogicalLeft,
      depthd::ImageProductFormat::kGray8};
  result.metadata.right_rectified = {
      grid, roi, depthd::CameraRole::kLogicalRight,
      depthd::ImageProductFormat::kGray8};
  result.metadata.disparity = {
      grid, roi, 0.5F, 8.0F, 0.0625F, true,
      depthd::ConfidenceModel::kBinarySgbmSupportV1,
      depthd::DisparityDerivation::kNativeMatcherGrid};
  result.metadata.depth = {grid, 0.1F, 10.0F};
  result.left_rectified = {1U, 2U};
  result.right_rectified = {3U, 4U};
  result.disparity = {1.0F, 2.0F};
  result.depth_m = {3.0F, 4.0F};
  result.confidence = {255U, 255U};
  result.validity = {1U, 1U};
  return result;
}

}  // namespace

int main() {
  const auto input = frame();
  const auto message = depthd::wire::make_frame_set_message(input);
  assert(message.header.kind == depthd::wire::MessageKind::kFrameSet);
  assert(message.payload.size() == input.payload_bytes());
  const std::string metadata(reinterpret_cast<const char*>(message.metadata.data()),
                             message.metadata.size());
  assert(metadata.find("\"depth_unit\":\"metres\"") != std::string::npos);
  assert(metadata.find("\"kind\":\"validity\"") != std::string::npos);
  const auto bytes = depthd::wire::serialize(message);
  const auto roundtrip = depthd::wire::deserialize(bytes);
  assert(roundtrip.metadata == message.metadata);
  assert(roundtrip.payload == message.payload);

  const auto images = depthd::wire::make_frame_set_message(
      input, depthd::ipc_v2::product_bit(
                 depthd::ipc_v2::ProductKind::kLeftRectified));
  assert(images.payload.size() == input.left_rectified.size());
}
