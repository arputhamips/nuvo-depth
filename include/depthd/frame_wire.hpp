#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "depthd/ipc_v2.hpp"
#include "depthd/runtime.hpp"
#include "depthd/wire_protocol.hpp"

namespace depthd::wire {

// Builds one self-describing frame-set message. Product offsets are relative
// to the payload, never to the file/socket stream, and omitted products have
// no descriptor. Every selected product retains the same frame identity.
[[nodiscard]] Message make_frame_set_message(
    const FrameSetStorage& frame,
    ipc_v2::ProductMask products = ipc_v2::kAllProducts,
    std::uint64_t stream_sequence = 1U);

[[nodiscard]] std::vector<std::byte> serialize(const Message& message);
[[nodiscard]] Message deserialize(std::span<const std::byte> bytes,
                                  const Limits& limits = {});

}  // namespace depthd::wire
