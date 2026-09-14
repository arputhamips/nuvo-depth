#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace depthd::wire {

inline constexpr std::size_t kHeaderBytes = 32U;
inline constexpr std::uint16_t kProtocolMajor = 1U;
inline constexpr std::uint16_t kProtocolMinor = 0U;
inline constexpr std::uint32_t kHardMaximumMetadataBytes = 64U * 1024U;
inline constexpr std::uint64_t kHardMaximumPayloadBytes = 64ULL * 1024ULL * 1024ULL;

enum class MessageKind : std::uint16_t {
  kHello = 1,
  kCapabilities = 2,
  kSubscribe = 3,
  kSubscriptionAdmitted = 4,
  kFrameSet = 5,
  kGap = 6,
  kError = 7,
  kPing = 8,
};

struct Limits {
  std::uint32_t maximum_metadata_bytes{kHardMaximumMetadataBytes};
  std::uint64_t maximum_payload_bytes{kHardMaximumPayloadBytes};
};

struct Header {
  std::uint16_t major{kProtocolMajor};
  std::uint16_t minor{kProtocolMinor};
  MessageKind kind{MessageKind::kHello};
  std::uint16_t flags{};
  std::uint32_t metadata_bytes{};
  std::uint64_t payload_bytes{};
  std::uint64_t stream_sequence{};
};

struct Message {
  Header header{};
  std::vector<std::byte> metadata;
  std::vector<std::byte> payload;

  [[nodiscard]] std::size_t wire_bytes() const;
};

void validate(const Limits& limits);
void validate(const Header& header, const Limits& limits = {});
void validate(const Message& message, const Limits& limits = {});

[[nodiscard]] std::array<std::byte, kHeaderBytes> encode_header(
    const Header& header, const Limits& limits = {});
[[nodiscard]] Header decode_header(std::span<const std::byte> bytes,
                                   const Limits& limits = {});

// Incremental decoder for TCP/TLS reads. feed() consumes no more than the
// current message, so callers can immediately take() and continue with any
// remaining bytes. It never allocates beyond the negotiated hard bounds.
class IncrementalDecoder final {
 public:
  explicit IncrementalDecoder(Limits limits = {});

  [[nodiscard]] std::size_t feed(std::span<const std::byte> bytes);
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] Message take();
  void reset() noexcept;

 private:
  Limits limits_{};
  std::array<std::byte, kHeaderBytes> header_bytes_{};
  std::size_t header_received_{};
  std::optional<Header> header_;
  std::vector<std::byte> metadata_;
  std::vector<std::byte> payload_;
};

}  // namespace depthd::wire
