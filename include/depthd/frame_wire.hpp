#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "depthd/ipc_v2.hpp"
#include "depthd/runtime.hpp"
#include "depthd/wire_protocol.hpp"

namespace depthd::wire {

// Payload views refer to the immutable frame. This layout allows local IPC to
// serialize directly into its final memfd without an intermediate payload.
struct FrameSetMessageParts {
  Header header{};
  std::vector<std::byte> metadata;
  std::array<std::span<const std::byte>, ipc_v2::kProductCount> products{};
  [[nodiscard]] std::size_t wire_bytes() const noexcept;
};
[[nodiscard]] FrameSetMessageParts prepare_frame_set_message(
    const FrameSetStorage& frame,
    ipc_v2::ProductMask products = ipc_v2::kAllProducts,
    std::uint64_t stream_sequence = 1U);
void write_frame_set_message(const FrameSetMessageParts& parts,
                             std::span<std::byte> destination);

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
