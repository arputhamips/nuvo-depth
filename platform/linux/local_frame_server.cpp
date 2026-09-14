#include "depthd/local_frame_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace depthd::ipc_v2 {
namespace {

constexpr std::array<std::byte, 4> kRequestMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'Q'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kResponseMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'R'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kErrorMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'E'}, std::byte{'2'}};

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

[[nodiscard]] std::array<std::byte, 8> request_packet(
    const ProductMask products) {
  std::array<std::byte, 8> result{};
  std::copy(kRequestMagic.begin(), kRequestMagic.end(), result.begin());
  put_u32(result.data() + 4U, products);
  return result;
}

void send_error(const SeqpacketConnection& connection,
                const std::string_view detail) {
  const auto count = std::min<std::size_t>(detail.size(), 1024U);
  std::vector<std::byte> response(kErrorMagic.size() + count);
  std::copy(kErrorMagic.begin(), kErrorMagic.end(), response.begin());
  if (count != 0U) {
    std::memcpy(response.data() + kErrorMagic.size(), detail.data(), count);
  }
  connection.send_packet(response);
}

}  // namespace

LocalFrameServer::LocalFrameServer(std::filesystem::path path,
                                   const std::uint16_t permissions)
    : listener_(std::move(path), permissions),
      worker_(&LocalFrameServer::run, this) {}

LocalFrameServer::~LocalFrameServer() noexcept { stop(); }

void LocalFrameServer::publish(std::shared_ptr<const FrameSetStorage> frame) {
  if (!frame) {
    throw std::invalid_argument("local frame publication cannot be null");
  }
  (void)frame->view();
  std::lock_guard lock(mutex_);
  if (stopping_) {
    throw std::logic_error("local frame server is stopped");
  }
  latest_ = std::move(frame);
}

void LocalFrameServer::stop() noexcept {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  if (worker_.joinable()) worker_.join();
}

void LocalFrameServer::run() noexcept {
  while (true) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) return;
    }
    try {
      auto connection = listener_.accept(std::chrono::milliseconds(100));
      if (connection) serve(std::move(connection));
    } catch (...) {
      // A malformed or disconnected local client cannot stop capture.
    }
  }
}

void LocalFrameServer::serve(SeqpacketConnection connection) {
  (void)connection.peer_credentials();
  const auto request = connection.receive_packet(
      8U, 0U, std::chrono::milliseconds(250));
  if (request.payload.size() != 8U ||
      !std::equal(kRequestMagic.begin(), kRequestMagic.end(),
                  request.payload.begin())) {
    send_error(connection, "invalid local frame request");
    return;
  }
  const auto products = get_u32(request.payload.data() + 4U);
  if (products == 0U || (products & ~kAllProducts) != 0U) {
    send_error(connection, "invalid local product mask");
    return;
  }
  std::shared_ptr<const FrameSetStorage> frame;
  {
    std::lock_guard lock(mutex_);
    frame = latest_;
  }
  if (!frame) {
    send_error(connection, "no frame available");
    return;
  }
  const auto bytes = wire::serialize(wire::make_frame_set_message(
      *frame, products, frame->metadata.frame.capture_sequence));
  auto memory = SealedSharedMemory::create("nuvo-local-frame", bytes.size());
  std::copy(bytes.begin(), bytes.end(), memory.writable_bytes().begin());
  std::array<std::byte, 16> response{};
  std::copy(kResponseMagic.begin(), kResponseMagic.end(), response.begin());
  put_u64(response.data() + 8U, bytes.size());
  const std::array handle{memory.readonly_handle()};
  connection.send_packet(response, handle);
}

wire::Message request_latest_local(const std::filesystem::path& path,
                                   const ProductMask products) {
#if defined(__linux__)
  if (products == 0U || (products & ~kAllProducts) != 0U) {
    throw std::invalid_argument("invalid local product mask");
  }
  auto connection = connect_seqpacket(path);
  const auto request = request_packet(products);
  connection.send_packet(request);
  auto response = connection.receive_packet(
      1032U, 1U, std::chrono::seconds(5));
  if (response.payload.size() >= 4U &&
      std::equal(kErrorMagic.begin(), kErrorMagic.end(),
                 response.payload.begin())) {
    throw std::runtime_error(std::string(
        reinterpret_cast<const char*>(response.payload.data() + 4U),
        response.payload.size() - 4U));
  }
  if (response.payload.size() != 16U || response.handles.size() != 1U ||
      !std::equal(kResponseMagic.begin(), kResponseMagic.end(),
                  response.payload.begin())) {
    throw std::runtime_error("invalid local frame response");
  }
  const auto size = get_u64(response.payload.data() + 8U);
  if (size == 0U || size > wire::kHeaderBytes +
                               wire::kHardMaximumMetadataBytes +
                               wire::kHardMaximumPayloadBytes) {
    throw std::length_error("local frame exceeds NuvoStream bounds");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  std::size_t offset{};
  while (offset < bytes.size()) {
    const auto read = ::pread(response.handles.front().get(),
                              bytes.data() + offset, bytes.size() - offset,
                              static_cast<off_t>(offset));
    if (read < 0) {
      throw std::runtime_error(std::string("read local frame: ") +
                               std::strerror(errno));
    }
    if (read == 0) throw std::runtime_error("local frame memfd is truncated");
    offset += static_cast<std::size_t>(read);
  }
  return wire::deserialize(bytes);
#else
  (void)path;
  (void)products;
  throw std::runtime_error("local frame service is available only on Linux");
#endif
}

}  // namespace depthd::ipc_v2
