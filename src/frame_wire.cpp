#include "depthd/frame_wire.hpp"

#include <array>
#include <cstring>
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

struct ProductSource {
  ipc_v2::ProductKind kind;
  const char* name;
  const char* format;
  const std::byte* data;
  std::size_t bytes;
  PixelGrid grid;
};

}  // namespace

Message make_frame_set_message(const FrameSetStorage& frame,
                               const ipc_v2::ProductMask products,
                               const std::uint64_t stream_sequence) {
  if (products == 0U || (products & ~ipc_v2::kAllProducts) != 0U ||
      stream_sequence == 0U) {
    throw std::invalid_argument("frame wire selection is invalid");
  }
  (void)frame.view();
  const auto& metadata = frame.metadata;
  const std::array sources{
      ProductSource{ipc_v2::ProductKind::kLeftRectified, "left_rectified",
                    "gray8",
                    reinterpret_cast<const std::byte*>(frame.left_rectified.data()),
                    frame.left_rectified.size(), metadata.left_rectified.grid},
      ProductSource{ipc_v2::ProductKind::kRightRectified, "right_rectified",
                    "gray8",
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
                    frame.validity.size(), metadata.disparity.grid}};

  Message result;
  result.header.kind = MessageKind::kFrameSet;
  result.header.stream_sequence = stream_sequence;
  std::ostringstream document;
  document << "{\"schema_version\":1,\"capture_epoch\":"
           << metadata.frame.capture_epoch << ",\"capture_sequence\":"
           << metadata.frame.capture_sequence
           << ",\"capture_time_ns\":" << metadata.frame.capture_time_ns
           << ",\"capture_clock\":\"boottime\",\"configuration_epoch\":"
           << metadata.frame.configuration_epoch << ",\"device_uuid\":\""
           << hex(metadata.frame.device_uuid) << "\",\"run_uuid\":\""
           << hex(metadata.frame.run_uuid) << "\",\"calibration_sha256\":\""
           << hex(metadata.calibration.sha256)
           << "\",\"calibration_authority\":"
           << static_cast<unsigned>(metadata.calibration_authority)
           << ",\"depth_unit\":\"metres\",\"depth_convention\":"
              "\"optical_axis_z\",\"products\":[";
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
    result.payload.insert(result.payload.end(), source.data,
                          source.data + source.bytes);
    offset += source.bytes;
  }
  document << "]}";
  const auto metadata_json = document.str();
  result.metadata.resize(metadata_json.size());
  std::memcpy(result.metadata.data(), metadata_json.data(), metadata_json.size());
  result.header.metadata_bytes =
      static_cast<std::uint32_t>(result.metadata.size());
  result.header.payload_bytes = result.payload.size();
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
