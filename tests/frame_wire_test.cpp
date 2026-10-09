#include "depthd/frame_wire.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "depthd/runtime.hpp"

namespace {

depthd::FrameSetStorage frame() {
  depthd::FrameSetStorage result;
  result.stereo_configuration = {7U,std::string(64U,'a'),std::string(64U,'b'),"invalid \"range\"\n"};
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

int main(const int argc, char** argv) {
  const auto input = frame();
  const auto message = depthd::wire::make_frame_set_message(input);
  assert(message.header.kind == depthd::wire::MessageKind::kFrameSet);
  assert(message.payload.size() == input.payload_bytes());
  const std::string metadata(reinterpret_cast<const char*>(message.metadata.data()),
                             message.metadata.size());
  assert(metadata.find("\"depth_unit\":\"metres\"") != std::string::npos);
  assert(metadata.find("\"kind\":\"validity\"") != std::string::npos);
  assert(metadata.find("\"stereo_configuration_generation\":7") != std::string::npos);
  assert(metadata.find("\"stereo_requested_revision\":\"" + std::string(64U,'a') + "\"") != std::string::npos);
  assert(metadata.find("\"stereo_applied_revision\":\"" + std::string(64U,'b') + "\"") != std::string::npos);
  assert(metadata.find("invalid \\\"range\\\"\\u000a") != std::string::npos);
  const auto bytes = depthd::wire::serialize(message);
  const auto roundtrip = depthd::wire::deserialize(bytes);
  assert(roundtrip.metadata == message.metadata);
  assert(roundtrip.payload == message.payload);
  const auto parts = depthd::wire::prepare_frame_set_message(input);
  std::vector<std::byte> direct(parts.wire_bytes());
  depthd::wire::write_frame_set_message(parts, direct);
  assert(direct == bytes);

  auto extended = frame();
  extended.available_products = depthd::ipc_v2::kSupportedProducts;
  extended.left_mono = {5U, 6U}; extended.right_mono = {7U, 8U};
  extended.left_rgb = {5U, 15U, 25U, 6U, 16U, 26U};
  extended.right_rgb = {7U, 17U, 27U, 8U, 18U, 28U};
  extended.left_rectified_rgb = extended.left_rgb;
  extended.right_rectified_rgb = extended.right_rgb;
  extended.rectified_stereo = {1U, 2U, 3U, 4U};
  extended.rectified_stereo_rgb = {5U, 15U, 25U, 6U, 16U, 26U, 7U, 17U, 27U, 8U, 18U, 28U};
  const auto selected_extended = depthd::wire::make_frame_set_message(extended, depthd::ipc_v2::kSupportedProducts);
  const auto extended_parts = depthd::wire::prepare_frame_set_message(extended, depthd::ipc_v2::kSupportedProducts);
  direct.resize(extended_parts.wire_bytes());
  depthd::wire::write_frame_set_message(extended_parts, direct);
  assert(direct == depthd::wire::serialize(selected_extended));
  if (argc == 2) {
    std::ofstream output(std::filesystem::path(argv[1]) / "extended.nuv", std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(direct.data()), static_cast<std::streamsize>(direct.size()));
    assert(output.good());
  }
  const std::string extended_metadata(reinterpret_cast<const char*>(selected_extended.metadata.data()),
                                      selected_extended.metadata.size());
  assert(extended_metadata.find("\"kind\":\"rectified_stereo_rgb\",\"format\":\"rgb8\"") != std::string::npos);
  assert(extended_metadata.find("\"length\":12,\"width\":4,\"height\":1") != std::string::npos);
  bool unavailable_rejected{};
  try { (void)depthd::wire::make_frame_set_message(input,
      depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kLeftRgb)); }
  catch (const std::invalid_argument&) { unavailable_rejected = true; }
  assert(unavailable_rejected);

  const auto images = depthd::wire::make_frame_set_message(
      input, depthd::ipc_v2::product_bit(
                 depthd::ipc_v2::ProductKind::kLeftRectified));
  assert(images.payload.size() == input.left_rectified.size());

  for (const auto& [format, name] :
       {std::pair{depthd::ImageProductFormat::kBgr8, "bgr8"},
        std::pair{depthd::ImageProductFormat::kRgb8, "rgb8"}}) {
    auto color = frame();
    color.metadata.left_rectified.format = format;
    color.metadata.right_rectified.format = format;
    color.left_rectified = {1U, 27U, 203U, 4U, 55U, 161U};
    color.right_rectified = {207U, 31U, 2U, 167U, 59U, 5U};
    const auto selected = depthd::wire::make_frame_set_message(
        color, depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kLeftRectified) |
                   depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kRightRectified));
    const std::string document(
        reinterpret_cast<const char*>(selected.metadata.data()),
        selected.metadata.size());
    assert(document.find(std::string("\"kind\":\"left_rectified\",\"format\":\"") +
                         name + "\",\"offset\":0,\"length\":6") != std::string::npos);
    assert(document.find(std::string("\"kind\":\"right_rectified\",\"format\":\"") +
                         name + "\",\"offset\":6,\"length\":6") != std::string::npos);
    const auto serialized = depthd::wire::serialize(selected);
    const auto decoded = depthd::wire::deserialize(serialized);
    assert(decoded.metadata == selected.metadata);
    assert(decoded.payload.size() == 12U);
    assert(std::memcmp(decoded.payload.data(), color.left_rectified.data(), 6U) == 0);
    assert(std::memcmp(decoded.payload.data() + 6U, color.right_rectified.data(), 6U) == 0);
    if (argc == 2) {
      const auto path = std::filesystem::path(argv[1]) / (std::string(name) + ".nuv");
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char*>(serialized.data()),
                   static_cast<std::streamsize>(serialized.size()));
      assert(output.good());
    }
  }
}
