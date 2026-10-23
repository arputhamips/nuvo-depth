#include "depthd/frame_wire.hpp"

#include <array>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace depthd::wire {
namespace {

[[nodiscard]] std::string hex(const std::span<const std::uint8_t> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto value : bytes) {
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
  }
  return result;
}

[[nodiscard]] std::string json_quote(const std::string_view value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result{"\""};
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') { result.push_back('\\'); result.push_back(static_cast<char>(byte)); }
    else if (byte < 32U) {
      result += "\\u00";
      result.push_back(digits[byte >> 4U]); result.push_back(digits[byte & 15U]);
    } else result.push_back(static_cast<char>(byte));
  }
  result.push_back('"');
  return result;
}

struct ProductSource {
  ipc_v2::ProductKind kind;
  const char* name;
  const char* format;
  const std::byte* data;
  std::size_t bytes;
  PixelGrid grid;
};

[[nodiscard]] const char* image_format_name(const ImageProductFormat format) {
  switch (format) {
    case ImageProductFormat::kGray8:
      return "gray8";
    case ImageProductFormat::kBgr8:
      return "bgr8";
    case ImageProductFormat::kRgb8:
      return "rgb8";
  }
  throw std::invalid_argument("unsupported frame wire image format");
}

void write_grid(std::ostream& document, const PixelGrid& grid) {
  document << "{\"width\":" << grid.width << ",\"height\":" << grid.height
           << ",\"reference_width\":" << grid.reference_width
           << ",\"reference_height\":" << grid.reference_height
           << ",\"x_scale_to_reference\":" << grid.x_scale_to_reference
           << ",\"y_scale_to_reference\":" << grid.y_scale_to_reference
           << ",\"x_offset_to_reference\":" << grid.x_offset_to_reference
           << ",\"y_offset_to_reference\":" << grid.y_offset_to_reference << '}';
}

template <std::size_t Size>
void write_matrix(std::ostream& document, const std::array<double, Size>& values) {
  document << '[';
  for (std::size_t index = 0; index < Size; ++index) {
    if (index != 0U) document << ',';
    document << values[index];
  }
  document << ']';
}

void write_calibration(std::ostream& document, const RectifiedCalibrationView& calibration) {
  validate(calibration);
  document << "{\"left_projection\":";
  write_matrix(document, calibration.left_projection);
  document << ",\"right_projection\":";
  write_matrix(document, calibration.right_projection);
  document << ",\"reprojection\":";
  write_matrix(document, calibration.reprojection_q);
  document << ",\"baseline_m\":" << calibration.baseline_m << '}';
}

void write_camera_capture(std::ostream& document, const CaptureCameraMetadata& capture) {
  const auto finite_or_null = [&](const double value) {
    if (std::isfinite(value) && value > 0.0) document << value;
    else document << "null";
  };
  document << "{\"sensor_timestamp_ns\":" << capture.sensor_timestamp_ns
           << ",\"stream_sequence\":" << capture.stream_sequence
           << ",\"reported_exposure_time_us\":";
  finite_or_null(capture.exposure_time_us);
  document << ",\"analogue_gain\":";
  finite_or_null(capture.analogue_gain);
  document << ",\"digital_gain\":";
  finite_or_null(capture.digital_gain);
  document << '}';
}

}  // namespace

FrameSetMessageParts prepare_frame_set_message(const FrameSetStorage& frame,
                               const ipc_v2::ProductMask products,
                               const std::uint64_t stream_sequence) {
  if (products == 0U || (products & ~ipc_v2::kSupportedProducts) != 0U ||
      (products & ~frame.available_products) != 0U ||
      stream_sequence == 0U) {
    throw std::invalid_argument("frame wire selection is invalid");
  }
  (void)frame.view();
  const auto& metadata = frame.metadata;
  const auto stereo_grid = make_reference_grid(metadata.left_rectified.grid.width * 2U,
                                              metadata.left_rectified.grid.height);
  const std::array sources{
      ProductSource{ipc_v2::ProductKind::kLeftRectified, "left_rectified",
                    image_format_name(metadata.left_rectified.format),
                    reinterpret_cast<const std::byte*>(frame.left_rectified.data()),
                    frame.left_rectified.size(), metadata.left_rectified.grid},
      ProductSource{ipc_v2::ProductKind::kRightRectified, "right_rectified",
                    image_format_name(metadata.right_rectified.format),
                    reinterpret_cast<const std::byte*>(frame.right_rectified.data()),
                    frame.right_rectified.size(), metadata.right_rectified.grid},
      ProductSource{ipc_v2::ProductKind::kDisparity, "disparity", "float32_le",
                    reinterpret_cast<const std::byte*>(frame.disparity.data()),
                    frame.disparity.size() * sizeof(float), metadata.disparity.grid},
      ProductSource{ipc_v2::ProductKind::kDepth, "depth", "float32_le",
                    reinterpret_cast<const std::byte*>(frame.depth_m.data()),
                    frame.depth_m.size() * sizeof(float), metadata.depth.grid},
      ProductSource{ipc_v2::ProductKind::kConfidence, "confidence", "uint8",
                    reinterpret_cast<const std::byte*>(frame.confidence.data()),
                    frame.confidence.size(), metadata.disparity.grid},
      ProductSource{ipc_v2::ProductKind::kValidity, "validity", "uint8",
                    reinterpret_cast<const std::byte*>(frame.validity.data()),
                    frame.validity.size(), metadata.disparity.grid},
      ProductSource{ipc_v2::ProductKind::kLeftMono, "left_mono", "gray8",
                    reinterpret_cast<const std::byte*>(frame.left_mono.data()),
                    frame.left_mono.size(), metadata.left_rectified.grid},
      ProductSource{ipc_v2::ProductKind::kRightMono, "right_mono", "gray8",
                    reinterpret_cast<const std::byte*>(frame.right_mono.data()),
                    frame.right_mono.size(), metadata.right_rectified.grid},
      ProductSource{ipc_v2::ProductKind::kLeftRgb, "left_rgb", "rgb8",
                    reinterpret_cast<const std::byte*>(frame.left_rgb.data()),
                    frame.left_rgb.size(), metadata.left_rectified.grid},
      ProductSource{ipc_v2::ProductKind::kRightRgb, "right_rgb", "rgb8",
                    reinterpret_cast<const std::byte*>(frame.right_rgb.data()),
                    frame.right_rgb.size(), metadata.right_rectified.grid},
      ProductSource{ipc_v2::ProductKind::kLeftRectifiedRgb, "left_rectified_rgb", "rgb8",
                    reinterpret_cast<const std::byte*>(frame.left_rectified_rgb.data()),
                    frame.left_rectified_rgb.size(), metadata.left_rectified.grid},
      ProductSource{ipc_v2::ProductKind::kRightRectifiedRgb, "right_rectified_rgb", "rgb8",
                    reinterpret_cast<const std::byte*>(frame.right_rectified_rgb.data()),
                    frame.right_rectified_rgb.size(), metadata.right_rectified.grid},
      ProductSource{ipc_v2::ProductKind::kRectifiedStereo, "rectified_stereo", "gray8",
                    reinterpret_cast<const std::byte*>(frame.rectified_stereo.data()),
                    frame.rectified_stereo.size(), stereo_grid},
      ProductSource{ipc_v2::ProductKind::kRectifiedStereoRgb, "rectified_stereo_rgb", "rgb8",
                    reinterpret_cast<const std::byte*>(frame.rectified_stereo_rgb.data()),
                    frame.rectified_stereo_rgb.size(), stereo_grid}};

  FrameSetMessageParts result;
  result.header.kind = MessageKind::kFrameSet;
  result.header.stream_sequence = stream_sequence;
  std::ostringstream document;
  document.imbue(std::locale::classic());
  document << std::setprecision(std::numeric_limits<double>::max_digits10);
  document << "{\"schema_version\":1,\"capture_epoch\":"
           << metadata.frame.capture_epoch << ",\"capture_sequence\":"
           << metadata.frame.capture_sequence
           << ",\"capture_time_ns\":" << metadata.frame.capture_time_ns
           << ",\"capture_clock\":\"boottime\",\"configuration_epoch\":"
           << metadata.frame.configuration_epoch
           << ",\"stereo_configuration_generation\":" << frame.stereo_configuration.generation
           << ",\"stereo_requested_revision\":" << json_quote(frame.stereo_configuration.requested_revision)
           << ",\"stereo_applied_revision\":" << json_quote(frame.stereo_configuration.applied_revision)
           << ",\"stereo_configuration_error\":" << json_quote(frame.stereo_configuration.rejection_error)
           << ",\"device_uuid\":\""
           << hex(metadata.frame.device_uuid) << "\",\"run_uuid\":\""
           << hex(metadata.frame.run_uuid) << "\",\"calibration_sha256\":\""
           << hex(metadata.calibration.sha256)
           << "\",\"calibration_authority\":"
           << static_cast<unsigned>(metadata.calibration_authority)
           << ",\"available_products\":" << frame.available_products
           << ",\"selected_products\":" << products
           << ",\"depth_unit\":\"metres\",\"depth_convention\":"
              "\"optical_axis_z\",\"image_grid\":";
  write_grid(document, metadata.left_rectified.grid);
  document << ",\"matching_grid\":";
  write_grid(document, metadata.disparity.grid);
  const auto& disparity = metadata.disparity;
  const char* confidence = disparity.confidence_model == ConfidenceModel::kBinarySgbmSupportV1 ?
      "binary_sgbm_support_v1" : disparity.confidence_model == ConfidenceModel::kBinaryBmSupportV1 ?
      "binary_bm_support_v1" : "none";
  document << ",\"disparity_metadata\":{\"valid_roi\":{\"x\":" << disparity.valid_roi.x
           << ",\"y\":" << disparity.valid_roi.y << ",\"width\":" << disparity.valid_roi.width
           << ",\"height\":" << disparity.valid_roi.height
           << "},\"unit\":\"matching_grid_pixels\",\"quantization_px\":" << disparity.algorithmic_quantization_px
           << ",\"accepted_min_px\":" << disparity.accepted_min_disparity_px
           << ",\"accepted_max_px\":" << disparity.accepted_max_disparity_px
           << ",\"invalid_is_nan\":" << (disparity.invalid_is_nan ? "true" : "false")
           << ",\"confidence_model\":\"" << confidence << "\",\"derivation\":\""
           << (disparity.derivation == DisparityDerivation::kNativeMatcherGrid ?
               "native_matcher_grid" : "invalid_aware_bilinear_compatibility") << "\"}"
           << ",\"depth_metadata\":{\"minimum_m\":" << metadata.depth.minimum_depth_m
           << ",\"maximum_m\":" << metadata.depth.maximum_depth_m
           << ",\"invalid_is_nan\":" << (metadata.depth.invalid_is_nan ? "true" : "false") << '}';
  if (frame.image_calibration && frame.matching_calibration) {
    validate(metadata, *frame.image_calibration, *frame.matching_calibration);
    document << ",\"calibration_geometry\":{\"image\":";
    write_calibration(document, *frame.image_calibration);
    document << ",\"matching\":";
    write_calibration(document, *frame.matching_calibration);
    document << '}';
  }
  if (frame.capture_metadata) {
    document << ",\"capture\":{\"sensor_delta_ns\":" << frame.capture_metadata->sensor_delta_ns
             << ",\"left\":";
    write_camera_capture(document, frame.capture_metadata->left);
    document << ",\"right\":";
    write_camera_capture(document, frame.capture_metadata->right);
    document << '}';
  }
  document << ",\"products\":[";
  std::size_t offset{};
  bool first = true;
  for (const auto& source : sources) {
    if ((products & ipc_v2::product_bit(source.kind)) == 0U) {
      continue;
    }
    if (!first) {
      document << ',';
    }
    first = false;
    document << "{\"kind\":\"" << source.name << "\",\"format\":\""
             << source.format << "\",\"offset\":" << offset
             << ",\"length\":" << source.bytes << ",\"width\":"
             << source.grid.width << ",\"height\":" << source.grid.height
             << '}';
    result.products[static_cast<std::size_t>(source.kind)] = {source.data, source.bytes};
    offset += source.bytes;
  }
  document << "]}";
  const auto metadata_json = document.str();
  result.metadata.resize(metadata_json.size());
  std::memcpy(result.metadata.data(), metadata_json.data(), metadata_json.size());
  result.header.metadata_bytes =
      static_cast<std::uint32_t>(result.metadata.size());
  result.header.payload_bytes = offset;
  (void)encode_header(result.header);
  return result;
}

std::size_t FrameSetMessageParts::wire_bytes() const noexcept {
  return kHeaderBytes + metadata.size() + static_cast<std::size_t>(header.payload_bytes);
}

void write_frame_set_message(const FrameSetMessageParts& parts,
                             const std::span<std::byte> destination) {
  validate(parts.header);
  std::size_t total_payload{};
  for (const auto product : parts.products) {
    if (product.size() > parts.header.payload_bytes - total_payload)
      throw std::invalid_argument("frame serializer product lengths are invalid");
    total_payload += product.size();
  }
  if (parts.header.metadata_bytes != parts.metadata.size() ||
      total_payload != parts.header.payload_bytes)
    throw std::invalid_argument("frame serializer part lengths are invalid");
  if (destination.size() != parts.wire_bytes())
    throw std::invalid_argument("frame serializer destination size is invalid");
  const auto header = encode_header(parts.header);
  auto* next = destination.data();
  std::memcpy(next, header.data(), header.size()); next += header.size();
  std::memcpy(next, parts.metadata.data(), parts.metadata.size()); next += parts.metadata.size();
  for (const auto product : parts.products) {
    if (!product.empty()) { std::memcpy(next, product.data(), product.size()); next += product.size(); }
  }
  if (next != destination.data() + destination.size())
    throw std::invalid_argument("frame serializer product lengths are invalid");
}

Message make_frame_set_message(const FrameSetStorage& frame,
                               const ipc_v2::ProductMask products,
                               const std::uint64_t stream_sequence) {
  const auto parts = prepare_frame_set_message(frame, products, stream_sequence);
  Message result;
  result.header = parts.header;
  result.metadata = parts.metadata;
  result.payload.resize(static_cast<std::size_t>(parts.header.payload_bytes));
  auto* next = result.payload.data();
  for (const auto product : parts.products) {
    if (!product.empty()) { std::memcpy(next, product.data(), product.size()); next += product.size(); }
  }
  validate(result);
  return result;
}

std::vector<std::byte> serialize(const Message& message) {
  validate(message);
  const auto header = encode_header(message.header);
  std::vector<std::byte> result;
  result.reserve(message.wire_bytes());
  result.insert(result.end(), header.begin(), header.end());
  result.insert(result.end(), message.metadata.begin(), message.metadata.end());
  result.insert(result.end(), message.payload.begin(), message.payload.end());
  return result;
}

Message deserialize(const std::span<const std::byte> bytes,
                    const Limits& limits) {
  if (bytes.size() < kHeaderBytes) {
    throw std::invalid_argument("serialized message is shorter than its header");
  }
  Message result;
  result.header = decode_header(bytes.first(kHeaderBytes), limits);
  const auto total = kHeaderBytes +
                     static_cast<std::size_t>(result.header.metadata_bytes) +
                     static_cast<std::size_t>(result.header.payload_bytes);
  if (total != bytes.size()) {
    throw std::invalid_argument("serialized message length does not match header");
  }
  auto remaining = bytes.subspan(kHeaderBytes);
  result.metadata.assign(
      remaining.begin(), remaining.begin() + result.header.metadata_bytes);
  remaining = remaining.subspan(result.header.metadata_bytes);
  result.payload.assign(remaining.begin(), remaining.end());
  validate(result, limits);
  return result;
}

}  // namespace depthd::wire
