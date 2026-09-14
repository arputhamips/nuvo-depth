#include "nuvo/depth.h"

#include <cassert>
#include <cstring>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "depthd/wire_protocol.hpp"

int main() {
  depthd::wire::Message message;
  message.header.kind = depthd::wire::MessageKind::kCapabilities;
  message.header.stream_sequence = 9U;
  const std::string json = "{}";
  message.metadata.assign(
      reinterpret_cast<const std::byte*>(json.data()),
      reinterpret_cast<const std::byte*>(json.data() + json.size()));
  message.header.metadata_bytes = static_cast<std::uint32_t>(json.size());
  const auto header = depthd::wire::encode_header(message.header);
  std::vector<std::uint8_t> bytes(header.size() + json.size());
  std::memcpy(bytes.data(), header.data(), header.size());
  std::memcpy(bytes.data() + header.size(), json.data(), json.size());

  std::unique_ptr<nuvo_stream_decoder, decltype(&nuvo_stream_decoder_destroy)>
      decoder(nuvo_stream_decoder_create(1024U, 1024U),
              nuvo_stream_decoder_destroy);
  if (!decoder || nuvo_depth_abi_version() != 1U ||
      nuvo_stream_decoder_feed(decoder.get(), bytes.data(), 7U) != 7U ||
      nuvo_stream_decoder_ready(decoder.get()) ||
      nuvo_stream_decoder_feed(decoder.get(), bytes.data() + 7U,
                               bytes.size() - 7U) != bytes.size() - 7U ||
      !nuvo_stream_decoder_ready(decoder.get()) ||
      nuvo_stream_decoder_sequence(decoder.get()) != 9U) {
    return 1;
  }
  size_t metadata_size{};
  const auto* metadata =
      nuvo_stream_decoder_metadata(decoder.get(), &metadata_size);
  if (metadata == nullptr || metadata_size != 2U || metadata[0] != '{') {
    return 1;
  }
  nuvo_stream_decoder_reset(decoder.get());
  return nuvo_stream_decoder_ready(decoder.get()) ? 1 : 0;
}
