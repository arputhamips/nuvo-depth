#include "depthd/wire_protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace depthd::wire {
namespace {

constexpr std::array<std::byte, 4> kMagic{
    std::byte{'N'}, std::byte{'U'}, std::byte{'V'}, std::byte{'1'}};

void put_u16(std::byte* destination, const std::uint16_t value) {
  destination[0] = static_cast<std::byte>((value >> 8U) & 0xffU);
  destination[1] = static_cast<std::byte>(value & 0xffU);
}

void put_u32(std::byte* destination, const std::uint32_t value) {
  for (std::size_t index = 0; index < 4U; ++index) {
    destination[index] =
        static_cast<std::byte>((value >> ((3U - index) * 8U)) & 0xffU);
  }
}

void put_u64(std::byte* destination, const std::uint64_t value) {
  for (std::size_t index = 0; index < 8U; ++index) {
    destination[index] =
        static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU);
  }
}

[[nodiscard]] std::uint16_t get_u16(const std::byte* source) {
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint16_t>(source[0]) << 8U) |
      std::to_integer<std::uint16_t>(source[1]));
}

[[nodiscard]] std::uint32_t get_u32(const std::byte* source) {
  std::uint32_t result{};
  for (std::size_t index = 0; index < 4U; ++index) {
    result = (result << 8U) | std::to_integer<std::uint32_t>(source[index]);
  }
  return result;
}

[[nodiscard]] std::uint64_t get_u64(const std::byte* source) {
  std::uint64_t result{};
  for (std::size_t index = 0; index < 8U; ++index) {
    result = (result << 8U) | std::to_integer<std::uint64_t>(source[index]);
  }
  return result;
}

[[nodiscard]] bool known_kind(const MessageKind kind) noexcept {
  switch (kind) {
    case MessageKind::kHello:
    case MessageKind::kCapabilities:
    case MessageKind::kSubscribe:
    case MessageKind::kSubscriptionAdmitted:
    case MessageKind::kFrameSet:
    case MessageKind::kGap:
    case MessageKind::kError:
    case MessageKind::kPing:
      return true;
  }
  return false;
}

}  // namespace

std::size_t Message::wire_bytes() const {
  if (payload.size() > std::numeric_limits<std::size_t>::max() -
                           metadata.size() - kHeaderBytes) {
    throw std::overflow_error("wire message byte size overflows");
  }
  return kHeaderBytes + metadata.size() + payload.size();
}

void validate(const Limits& limits) {
  if (limits.maximum_metadata_bytes == 0U ||
      limits.maximum_metadata_bytes > kHardMaximumMetadataBytes ||
      limits.maximum_payload_bytes == 0U ||
      limits.maximum_payload_bytes > kHardMaximumPayloadBytes) {
    throw std::invalid_argument("NuvoStream negotiated limits are invalid");
  }
}

void validate(const Header& header, const Limits& limits) {
  validate(limits);
  if (header.major != kProtocolMajor || header.minor > kProtocolMinor) {
    throw std::invalid_argument("unsupported NuvoStream protocol version");
  }
  if (!known_kind(header.kind) || header.flags != 0U ||
      header.metadata_bytes > limits.maximum_metadata_bytes ||
      header.payload_bytes > limits.maximum_payload_bytes ||
      header.stream_sequence == 0U) {
    throw std::invalid_argument("NuvoStream header is invalid or exceeds bounds");
  }
  if (header.kind == MessageKind::kFrameSet &&
      (header.metadata_bytes == 0U || header.payload_bytes == 0U)) {
    throw std::invalid_argument(
        "NuvoStream frame set requires metadata and product payload");
  }
  if (header.kind != MessageKind::kFrameSet && header.payload_bytes != 0U) {
    throw std::invalid_argument(
        "NuvoStream control messages cannot carry product payload");
  }
}

void validate(const Message& message, const Limits& limits) {
  validate(message.header, limits);
  if (message.metadata.size() != message.header.metadata_bytes ||
      message.payload.size() != message.header.payload_bytes) {
    throw std::invalid_argument(
        "NuvoStream message lengths disagree with its header");
  }
  (void)message.wire_bytes();
}

std::array<std::byte, kHeaderBytes> encode_header(const Header& header,
                                                  const Limits& limits) {
  validate(header, limits);
  std::array<std::byte, kHeaderBytes> result{};
  std::copy(kMagic.begin(), kMagic.end(), result.begin());
  put_u16(result.data() + 4U, header.major);
  put_u16(result.data() + 6U, header.minor);
  put_u16(result.data() + 8U, static_cast<std::uint16_t>(header.kind));
  put_u16(result.data() + 10U, header.flags);
  put_u32(result.data() + 12U, header.metadata_bytes);
  put_u64(result.data() + 16U, header.payload_bytes);
  put_u64(result.data() + 24U, header.stream_sequence);
  return result;
}

Header decode_header(const std::span<const std::byte> bytes,
                     const Limits& limits) {
  if (bytes.size() != kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    throw std::invalid_argument("NuvoStream header magic or size is invalid");
  }
  Header result;
  result.major = get_u16(bytes.data() + 4U);
  result.minor = get_u16(bytes.data() + 6U);
  result.kind = static_cast<MessageKind>(get_u16(bytes.data() + 8U));
  result.flags = get_u16(bytes.data() + 10U);
  result.metadata_bytes = get_u32(bytes.data() + 12U);
  result.payload_bytes = get_u64(bytes.data() + 16U);
  result.stream_sequence = get_u64(bytes.data() + 24U);
  validate(result, limits);
  return result;
}

IncrementalDecoder::IncrementalDecoder(const Limits limits) : limits_(limits) {
  validate(limits_);
}

std::size_t IncrementalDecoder::feed(const std::span<const std::byte> bytes) {
  if (ready()) {
    return 0U;
  }
  std::size_t consumed{};
  if (!header_) {
    const auto amount = std::min(kHeaderBytes - header_received_, bytes.size());
    std::copy_n(bytes.data(), amount,
                header_bytes_.data() + header_received_);
    header_received_ += amount;
    consumed += amount;
    if (header_received_ != kHeaderBytes) {
      return consumed;
    }
    header_ = decode_header(header_bytes_, limits_);
    metadata_.reserve(header_->metadata_bytes);
    payload_.reserve(static_cast<std::size_t>(header_->payload_bytes));
  }
  const auto remainder = bytes.subspan(consumed);
  const auto metadata_amount =
      std::min(static_cast<std::size_t>(header_->metadata_bytes) -
                   metadata_.size(),
               remainder.size());
  metadata_.insert(metadata_.end(), remainder.begin(),
                   remainder.begin() +
                       static_cast<std::ptrdiff_t>(metadata_amount));
  consumed += metadata_amount;
  if (metadata_.size() != header_->metadata_bytes) {
    return consumed;
  }
  const auto payload_remainder = bytes.subspan(consumed);
  const auto payload_target = static_cast<std::size_t>(header_->payload_bytes);
  const auto payload_amount =
      std::min(payload_target - payload_.size(), payload_remainder.size());
  payload_.insert(payload_.end(), payload_remainder.begin(),
                  payload_remainder.begin() +
                      static_cast<std::ptrdiff_t>(payload_amount));
  consumed += payload_amount;
  return consumed;
}

bool IncrementalDecoder::ready() const noexcept {
  return header_.has_value() && metadata_.size() == header_->metadata_bytes &&
         payload_.size() == header_->payload_bytes;
}

Message IncrementalDecoder::take() {
  if (!ready()) {
    throw std::logic_error("NuvoStream decoder has no complete message");
  }
  Message result{*header_, std::move(metadata_), std::move(payload_)};
  reset();
  return result;
}

void IncrementalDecoder::reset() noexcept {
  header_received_ = 0U;
  header_.reset();
  metadata_.clear();
  payload_.clear();
}

}  // namespace depthd::wire
