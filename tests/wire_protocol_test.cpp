#include "nuvo/stream_decoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

depthd::wire::Message message(const std::uint64_t sequence) {
  depthd::wire::Message result;
  result.header.kind = depthd::wire::MessageKind::kFrameSet;
  result.header.stream_sequence = sequence;
  result.metadata.assign(7U, std::byte{0x11});
  result.payload.assign(13U, std::byte{0x22});
  result.header.metadata_bytes =
      static_cast<std::uint32_t>(result.metadata.size());
  result.header.payload_bytes = result.payload.size();
  return result;
}

std::vector<std::byte> encode(const depthd::wire::Message& source) {
  const auto header = depthd::wire::encode_header(source.header);
  std::vector<std::byte> result(header.begin(), header.end());
  result.insert(result.end(), source.metadata.begin(), source.metadata.end());
  result.insert(result.end(), source.payload.begin(), source.payload.end());
  return result;
}

void test_exact_header_and_round_trip() {
  const auto source = message(0x0102030405060708ULL);
  const auto bytes = depthd::wire::encode_header(source.header);
  require(bytes[0] == std::byte{'N'} && bytes[3] == std::byte{'1'} &&
              bytes[8] == std::byte{0} && bytes[9] == std::byte{5} &&
              bytes[12] == std::byte{0} && bytes[15] == std::byte{7} &&
              bytes[24] == std::byte{1} && bytes[31] == std::byte{8},
          "NuvoStream header is not explicitly network-byte-order encoded");
  const auto decoded = depthd::wire::decode_header(bytes);
  require(decoded.kind == depthd::wire::MessageKind::kFrameSet &&
              decoded.metadata_bytes == 7U && decoded.payload_bytes == 13U &&
              decoded.stream_sequence == 0x0102030405060708ULL,
          "NuvoStream header round trip failed");
}

void test_incremental_sdk_decoder_and_coalesced_input() {
  const auto first = encode(message(1U));
  const auto second = encode(message(2U));
  std::vector<std::byte> joined = first;
  joined.insert(joined.end(), second.begin(), second.end());
  nuvo::StreamDecoder decoder;
  std::size_t offset{};
  const auto consumed_first = decoder.feed(joined);
  require(consumed_first == first.size() && decoder.ready(),
          "SDK decoder consumed beyond one framed message");
  auto decoded_first = decoder.take();
  require(decoded_first.header.stream_sequence == 1U &&
              decoded_first.metadata.size() == 7U &&
              decoded_first.payload.size() == 13U,
          "SDK decoder lost first message data");
  offset += consumed_first;
  while (offset < joined.size()) {
    const auto consumed = decoder.feed(
        std::span<const std::byte>(joined).subspan(offset, 1U));
    require(consumed == 1U, "byte-wise SDK decode made no progress");
    offset += consumed;
  }
  require(decoder.ready() && decoder.take().header.stream_sequence == 2U,
          "SDK decoder failed on byte-wise TCP fragmentation");
}

void test_strict_bounds_and_header_fuzz() {
  auto source = message(1U);
  auto bytes = depthd::wire::encode_header(source.header);
  bool rejected = false;
  try {
    (void)depthd::wire::decode_header(
        std::span<const std::byte>(bytes).first(bytes.size() - 1U));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "truncated NuvoStream header was accepted");
  bytes[0] = std::byte{'X'};
  rejected = false;
  try {
    (void)depthd::wire::decode_header(bytes);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "foreign NuvoStream magic was accepted");

  std::mt19937_64 random(123U);
  for (unsigned iteration = 0; iteration < 10'000U; ++iteration) {
    std::array<std::byte, depthd::wire::kHeaderBytes> fuzz{};
    for (auto& value : fuzz) {
      value = static_cast<std::byte>(random() & 0xffU);
    }
    try {
      (void)depthd::wire::decode_header(fuzz);
    } catch (const std::invalid_argument&) {
    }
  }

  auto oversized = message(3U).header;
  oversized.payload_bytes = depthd::wire::kHardMaximumPayloadBytes + 1U;
  rejected = false;
  try {
    (void)depthd::wire::encode_header(oversized);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "oversized NuvoStream payload was accepted");
}

}  // namespace

int main() {
  try {
    test_exact_header_and_round_trip();
    test_incremental_sdk_decoder_and_coalesced_input();
    test_strict_bounds_and_header_fuzz();
    std::cout << "wire_protocol_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "wire_protocol_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
