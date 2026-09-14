#pragma once

#include <cstddef>
#include <span>

#include "depthd/wire_protocol.hpp"

namespace nuvo {

using StreamLimits = depthd::wire::Limits;
using StreamHeader = depthd::wire::Header;
using StreamMessage = depthd::wire::Message;
using StreamMessageKind = depthd::wire::MessageKind;

// SDK-facing, allocation-bounded decoder for bytes already authenticated and
// decrypted by a host TLS implementation. Ownership of returned vectors stays
// with StreamMessage, so views can retain one received frame-set owner.
class StreamDecoder final {
 public:
  explicit StreamDecoder(StreamLimits limits = {}) : decoder_(limits) {}

  [[nodiscard]] std::size_t feed(std::span<const std::byte> bytes) {
    return decoder_.feed(bytes);
  }
  [[nodiscard]] bool ready() const noexcept { return decoder_.ready(); }
  [[nodiscard]] StreamMessage take() { return decoder_.take(); }
  void reset() noexcept { decoder_.reset(); }

 private:
  depthd::wire::IncrementalDecoder decoder_;
};

}  // namespace nuvo
