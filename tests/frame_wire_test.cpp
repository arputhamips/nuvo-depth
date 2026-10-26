#include "depthd/frame_wire.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <string>
#include <utility>

#include "depthd/runtime.hpp"

namespace {

depthd::FrameSetStorage frame() {
  depthd::FrameSetStorage result;
  result.stereo_configuration = {7U,std::string(64U,'a'),std::string(64U,'b'),"invalid \"range\"\n", {}};
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
  assert(metadata.find("\"matching_grid\":{\"width\":2,\"height\":1") != std::string::npos);
  assert(metadata.find("\"quantization_px\":0.0625") != std::string::npos);
  assert(metadata.find("\"confidence_model\":\"binary_sgbm_support_v1\"") != std::string::npos);
  assert(metadata.find("\"valid_roi\":{\"x\":0,\"y\":0,\"width\":2,\"height\":1}") != std::string::npos);
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

  auto with_geometry = frame();
  const std::array<double, 12> left_projection{100, 0, 0.5, 0, 0, 100, 0, 0, 0, 0, 1, 0};
  auto right_projection = left_projection;
  right_projection[3] = -10.0;
  with_geometry.image_calibration = depthd::make_rectified_calibration(
      with_geometry.metadata.calibration, with_geometry.metadata.calibration_authority,
      with_geometry.metadata.left_rectified.grid, left_projection, right_projection);
  with_geometry.matching_calibration = with_geometry.image_calibration;
  with_geometry.capture_metadata = depthd::CaptureFrameMetadata{
      {100, 10, 7000, 1.5, 1.0}, {120, 11, 7000, 1.5, 1.0}, 20};
  const auto geometry_message = depthd::wire::make_frame_set_message(with_geometry);
  const std::string geometry_json(reinterpret_cast<const char*>(geometry_message.metadata.data()),
                                 geometry_message.metadata.size());
  assert(geometry_json.find("\"calibration_geometry\":{\"image\":{\"left_projection\":[100,0,0.5") != std::string::npos);
  assert(geometry_json.find("\"reprojection\":[") != std::string::npos);
  assert(geometry_json.find("\"capture\":{\"sensor_delta_ns\":20") != std::string::npos);
  assert(geometry_json.find("\"reported_exposure_time_us\":7000") != std::string::npos);
  assert(geometry_message.payload == message.payload);
  if (argc == 2) {
    // Export a physically consistent modern frame for the canonical SDK test;
    // the serializer assertions above deliberately exercise arbitrary samples.
    auto sdk_frame = with_geometry;
    sdk_frame.stereo_configuration.rejection_error.clear();
    sdk_frame.disparity = {10.0F / 3.0F, 2.5F};
    const auto native_bytes = depthd::wire::serialize(depthd::wire::make_frame_set_message(sdk_frame));
    std::ofstream output(std::filesystem::path(argv[1]) / "geometry.nuv", std::ios::binary);
    output.write(reinterpret_cast<const char*>(native_bytes.data()), static_cast<std::streamsize>(native_bytes.size()));
    assert(output.good());
  }
  with_geometry.capture_metadata->left.analogue_gain = std::numeric_limits<double>::quiet_NaN();
  with_geometry.capture_metadata->right.exposure_time_us = 0.0;
  struct CommaDecimal final : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
  };
  const auto previous_locale = std::locale();
  std::locale::global(std::locale(previous_locale, new CommaDecimal));
  const auto missing_message = depthd::wire::make_frame_set_message(with_geometry);
  std::locale::global(previous_locale);
  const std::string missing_json(reinterpret_cast<const char*>(missing_message.metadata.data()),
                                 missing_message.metadata.size());
  assert(missing_json.find("\"analogue_gain\":null") != std::string::npos);
  assert(missing_json.find("\"reported_exposure_time_us\":null") != std::string::npos);
  assert(missing_json.find("\"quantization_px\":0.0625") != std::string::npos);

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
