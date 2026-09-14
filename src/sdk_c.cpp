#include "nuvo/depth.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>

#include "depthd/wire_protocol.hpp"

struct nuvo_stream_decoder {
  explicit nuvo_stream_decoder(const depthd::wire::Limits limits)
      : decoder(limits) {}
  depthd::wire::IncrementalDecoder decoder;
  std::optional<depthd::wire::Message> message;
  std::string error;
};

extern "C" {

uint32_t nuvo_depth_abi_version(void) { return NUVO_DEPTH_ABI_VERSION; }

nuvo_stream_decoder* nuvo_stream_decoder_create(
    const uint32_t maximum_metadata_bytes,
    const uint64_t maximum_payload_bytes) {
  try {
    return new nuvo_stream_decoder(
        {maximum_metadata_bytes, maximum_payload_bytes});
  } catch (...) {
    return nullptr;
  }
}

void nuvo_stream_decoder_destroy(nuvo_stream_decoder* decoder) {
  delete decoder;
}

size_t nuvo_stream_decoder_feed(nuvo_stream_decoder* decoder,
                                const uint8_t* bytes, const size_t size) {
  if (decoder == nullptr || (bytes == nullptr && size != 0U)) return 0U;
  try {
    decoder->error.clear();
    if (decoder->message) return 0U;
    const auto consumed = decoder->decoder.feed(
        {reinterpret_cast<const std::byte*>(bytes), size});
    if (decoder->decoder.ready()) decoder->message = decoder->decoder.take();
    return consumed;
  } catch (const std::exception& error) {
    decoder->error = error.what();
    return 0U;
  } catch (...) {
    decoder->error = "unknown decoder failure";
    return 0U;
  }
}

int nuvo_stream_decoder_ready(const nuvo_stream_decoder* decoder) {
  return decoder != nullptr && decoder->message.has_value();
}

uint16_t nuvo_stream_decoder_kind(const nuvo_stream_decoder* decoder) {
  return decoder != nullptr && decoder->message
             ? static_cast<uint16_t>(decoder->message->header.kind)
             : 0U;
}

uint64_t nuvo_stream_decoder_sequence(const nuvo_stream_decoder* decoder) {
  return decoder != nullptr && decoder->message
             ? decoder->message->header.stream_sequence
             : 0U;
}

const uint8_t* nuvo_stream_decoder_metadata(const nuvo_stream_decoder* decoder,
                                            size_t* size) {
  if (size != nullptr) *size = 0U;
  if (decoder == nullptr || !decoder->message || size == nullptr) return nullptr;
  *size = decoder->message->metadata.size();
  return reinterpret_cast<const uint8_t*>(decoder->message->metadata.data());
}

const uint8_t* nuvo_stream_decoder_payload(const nuvo_stream_decoder* decoder,
                                           size_t* size) {
  if (size != nullptr) *size = 0U;
  if (decoder == nullptr || !decoder->message || size == nullptr) return nullptr;
  *size = decoder->message->payload.size();
  return reinterpret_cast<const uint8_t*>(decoder->message->payload.data());
}

void nuvo_stream_decoder_reset(nuvo_stream_decoder* decoder) {
  if (decoder == nullptr) return;
  decoder->message.reset();
  decoder->decoder.reset();
  decoder->error.clear();
}

const char* nuvo_stream_decoder_last_error(const nuvo_stream_decoder* decoder) {
  return decoder == nullptr ? "decoder is null" : decoder->error.c_str();
}

}  // extern "C"
