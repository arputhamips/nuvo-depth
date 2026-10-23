#include "depthd/local_frame_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <future>
#include <cmath>
#include <functional>
#include <stdexcept>

#if defined(__linux__)
#include <unistd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

namespace depthd::ipc_v2 {
namespace {

constexpr std::array<std::byte, 4> kRequestMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'Q'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kResponseMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'R'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kErrorMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'E'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kWaitMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'W'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kNoFrameMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'N'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kSubscribeMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'S'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kAckMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'A'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kEncodedWaitMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'Y'}, std::byte{'2'}};
constexpr std::array<std::byte, 4> kInvalidatedMagic{
    std::byte{'N'}, std::byte{'V'}, std::byte{'I'}, std::byte{'2'}};
constexpr std::size_t kCacheMaximumBytes = 64U * 1024U * 1024U;
constexpr std::size_t kCacheMaximumEntries = 4U;

[[nodiscard]] LocalFrameCursor cursor(const FrameSetStorage& frame) {
  return {frame.metadata.frame.run_uuid, frame.metadata.frame.capture_epoch,
          frame.metadata.frame.capture_sequence};
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
  const auto same_scope = [](const auto& a, const auto& b) {
    return a.run_uuid == b.run_uuid && a.capture_epoch == b.capture_epoch &&
           a.configuration_epoch == b.configuration_epoch;
  };
  const auto& identity = frame->metadata.frame;
  if (latest_ && !same_scope(identity, latest_->metadata.frame)) {
    // A slow completion from a superseded configuration must not resurrect it.
    const auto& current = latest_->metadata.frame;
    if (identity.capture_time_ns < current.capture_time_ns ||
        (identity.run_uuid == current.run_uuid &&
         identity.capture_epoch == current.capture_epoch &&
         identity.configuration_epoch < current.configuration_epoch)) return;
    latest_.reset();
    latest_matcher_.reset();
  }
  if ((frame->available_products & kMatcherProducts) != 0U &&
      (!latest_matcher_ || identity.capture_sequence >
                              latest_matcher_->metadata.frame.capture_sequence))
    latest_matcher_ = frame;
  if (!latest_ || identity.capture_sequence >= latest_->metadata.frame.capture_sequence)
    latest_ = std::move(frame);
  for (auto entry = cache_.begin(); entry != cache_.end();) {
    const auto cached = entry->frame.lock();
    if (cached && (cached == latest_ || cached == latest_matcher_)) {
      ++entry;
    } else {
      cache_bytes_ -= entry->memory.size();
      entry = cache_.erase(entry);
    }
  }
  changed_.notify_all();
}

void LocalFrameServer::stop() noexcept {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
#if defined(__linux__)
    for (const auto& [id, client] : clients_) {
      (void)id;
      if (!client.waiting) (void)::shutdown(client.socket, SHUT_RDWR);
    }
#endif
    changed_.notify_all();
  }
  if (worker_.joinable()) worker_.join();
}

void LocalFrameServer::publish_encoded(const LocalFrameCursor& identity,
                                       std::span<const std::byte> bytes) {
  if (bytes.size() > 4U * 1024U * 1024U) throw std::length_error("encoded local frame exceeds bounds");
  std::shared_ptr<EncodedPublication> value;
  if (!bytes.empty()) {
    value = std::make_shared<EncodedPublication>();
    value->cursor = identity;
    value->memory = SealedSharedMemory::create("nuvo-local-preview", bytes.size());
    std::copy(bytes.begin(), bytes.end(), value->memory.writable_bytes().begin());
  }
  std::lock_guard lock(mutex_);
  encoded_ = std::move(value); changed_.notify_all();
}

LocalFrameDemand LocalFrameServer::demand() const noexcept {
  std::lock_guard lock(mutex_);
  return demand_;
}

void LocalFrameServer::recompute_demand_locked() noexcept {
  LocalFrameDemand next{};
  for (const auto& [id, client] : clients_) {
    (void)id;
    next.products |= client.products;
    next.cadence_hz = std::max(next.cadence_hz, client.cadence_hz);
    next.preview_cadence_hz = std::max(next.preview_cadence_hz, client.preview_cadence_hz);
    if ((client.products & kMatcherProducts) != 0U) {
      next.matcher_products |= client.products;
      next.matcher_cadence_hz = std::max(next.matcher_cadence_hz, client.cadence_hz);
    } else {
      next.image_products |= client.products;
      next.image_cadence_hz = std::max(next.image_cadence_hz, client.cadence_hz);
    }
  }
  next.generation = demand_.generation;
  if (next.products != demand_.products || next.cadence_hz != demand_.cadence_hz ||
      next.preview_cadence_hz != demand_.preview_cadence_hz ||
      next.image_products != demand_.image_products || next.image_cadence_hz != demand_.image_cadence_hz ||
      next.matcher_products != demand_.matcher_products || next.matcher_cadence_hz != demand_.matcher_cadence_hz) ++next.generation;
  demand_ = next;
  if (next.matcher_products == 0U) latest_matcher_.reset();
}

void LocalFrameServer::run() noexcept {
  std::vector<std::future<void>> clients;
  while (true) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) return;
    }
    try {
      std::erase_if(clients, [](auto& client) {
        return client.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
      });
      auto connection = listener_.accept(std::chrono::milliseconds(100));
      if (connection && clients.size() < 8U) {
        clients.push_back(std::async(std::launch::async,
            [this, connection = std::move(connection)]() mutable {
              try { serve(std::move(connection)); } catch (...) { }
            }));
      }
    } catch (...) {
      // A malformed or disconnected local client cannot stop capture.
    }
  }
}

void LocalFrameServer::serve(SeqpacketConnection connection) {
  (void)connection.peer_credentials();
  std::uint64_t client_id{};
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    client_id = next_client_id_++;
    clients_.emplace(client_id, ClientDemand{connection.native_handle()});
  }
  const auto cleanup = std::shared_ptr<void>(nullptr, [this, client_id](void*) {
    std::lock_guard lock(mutex_);
    clients_.erase(client_id);
    recompute_demand_locked();
  });
  bool persistent{};
  while (true) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
  }
  const auto request = connection.receive_packet(48U, 0U,
      persistent ? std::chrono::seconds(10) : std::chrono::milliseconds(250));
  if (request.payload.size() == 20U &&
      std::equal(kSubscribeMagic.begin(), kSubscribeMagic.end(), request.payload.begin())) {
    const auto products = get_u32(request.payload.data() + 4U);
    const auto cadence = get_u32(request.payload.data() + 8U);
    const auto preview = get_u32(request.payload.data() + 12U);
    if ((products & ~kSupportedProducts) != 0U || cadence > 60000U || preview > 60000U ||
        (products == 0U) != (cadence == 0U) || get_u32(request.payload.data() + 16U) != 0U) {
      send_error(connection, "invalid local demand bounds"); return;
    }
    {
      std::lock_guard lock(mutex_);
      auto& client = clients_.at(client_id);
      client.products = products;
      client.cadence_hz = static_cast<double>(cadence) / 1000.0;
      client.preview_cadence_hz = static_cast<double>(preview) / 1000.0;
      recompute_demand_locked();
    }
    persistent = true;
    connection.send_packet(kAckMagic);
    continue;
  }
  if (request.payload.size() == 48U &&
      std::equal(kEncodedWaitMagic.begin(), kEncodedWaitMagic.end(), request.payload.begin())) {
    const auto wait_ms = get_u32(request.payload.data() + 8U);
    LocalFrameCursor after{};
    std::memcpy(after.run_uuid.data(), request.payload.data() + 16U, 16U);
    after.capture_epoch = get_u64(request.payload.data() + 32U);
    after.capture_sequence = get_u64(request.payload.data() + 40U);
    std::unique_lock lock(mutex_);
    if (!persistent || clients_.at(client_id).preview_cadence_hz <= 0.0 ||
        wait_ms > 500U || get_u32(request.payload.data() + 4U) != 0U ||
        get_u32(request.payload.data() + 12U) != 0U) {
      lock.unlock(); send_error(connection, "invalid encoded wait bounds"); return;
    }
    clients_.at(client_id).waiting = true;
    changed_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] {
      return stopping_ || (encoded_ && encoded_->cursor != after) ||
          (!encoded_ && after != LocalFrameCursor{});
    });
    clients_.at(client_id).waiting = false;
    const auto value = encoded_;
    const bool stopped = stopping_;
    lock.unlock();
    if (stopped || !value || value->cursor == after) {
      connection.send_packet(!value ? kInvalidatedMagic : kNoFrameMagic);
      if (stopped) return;
      continue;
    }
    std::array<std::byte, 48> response{};
    std::copy(kResponseMagic.begin(), kResponseMagic.end(), response.begin());
    put_u64(response.data() + 8U, value->memory.size());
    std::memcpy(response.data() + 16U, value->cursor.run_uuid.data(), 16U);
    put_u64(response.data() + 32U, value->cursor.capture_epoch);
    put_u64(response.data() + 40U, value->cursor.capture_sequence);
    const std::array handle{value->memory.readonly_handle()};
    connection.send_packet(response, handle);
    continue;
  }
  const bool wait = request.payload.size() == 48U &&
      std::equal(kWaitMagic.begin(), kWaitMagic.end(), request.payload.begin());
  if (!wait && (request.payload.size() != 8U ||
      !std::equal(kRequestMagic.begin(), kRequestMagic.end(),
                  request.payload.begin()))) {
    send_error(connection, "invalid local frame request");
    return;
  }
  const auto products = get_u32(request.payload.data() + 4U);
  if (products == 0U || (products & ~kSupportedProducts) != 0U) {
    send_error(connection, "invalid local product mask");
    return;
  }
  {
    std::lock_guard lock(mutex_);
    auto& client = clients_.at(client_id);
    if (persistent && (products & ~client.products) != 0U) {
      send_error(connection, "wait products exceed registered demand"); return;
    }
    if (!persistent) {
      client.products = products;
      client.cadence_hz = 10.0;
      recompute_demand_locked();
    }
  }
  std::shared_ptr<const FrameSetStorage> frame;
  LocalFrameCursor after{};
  std::uint32_t wait_ms{};
  if (wait) {
    wait_ms = get_u32(request.payload.data() + 8U);
    if (wait_ms > 500U || get_u32(request.payload.data() + 12U) != 0U) {
      send_error(connection, "invalid local frame wait bound");
      return;
    }
    std::memcpy(after.run_uuid.data(), request.payload.data() + 16U, 16U);
    after.capture_epoch = get_u64(request.payload.data() + 32U);
    after.capture_sequence = get_u64(request.payload.data() + 40U);
  }
  {
    std::unique_lock lock(mutex_);
    const auto selected_frame = [&]() -> std::shared_ptr<const FrameSetStorage> {
      if (latest_ && (products & ~latest_->available_products) == 0U) return latest_;
      if (latest_matcher_ && (products & ~latest_matcher_->available_products) == 0U) return latest_matcher_;
      return {};
    };
    if (wait) {
      clients_.at(client_id).waiting = true;
      changed_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] {
        const auto candidate = selected_frame();
        return stopping_ || (candidate && cursor(*candidate) != after);
      });
      clients_.at(client_id).waiting = false;
      const auto candidate = selected_frame();
      if (stopping_ || !candidate || cursor(*candidate) == after) {
        const bool stopped = stopping_;
        lock.unlock();
        connection.send_packet(kNoFrameMagic);
        if (stopped || !persistent) return;
        continue;
      }
    }
    frame = selected_frame();
  }
  if (!frame) {
    send_error(connection, "no frame available");
    return;
  }
  if ((products & ~frame->available_products) != 0U) {
    send_error(connection, "selected local products are not available"); return;
  }
  std::unique_lock serialization_lock(serialization_mutex_);
  std::unique_lock lock(mutex_);
  // Publication only swaps a pointer. Serializing outside the producer lock
  // prevents a slow local reader from holding up the capture thread.
  auto found = std::find_if(cache_.begin(), cache_.end(), [&](auto& item) {
    return item.products == products && item.frame.lock() == frame;
  });
  SealedSharedMemory temporary;
  const SealedSharedMemory* memory{};
  if (found != cache_.end()) {
    memory = &found->memory;
  } else {
    lock.unlock();
    const auto parts = wire::prepare_frame_set_message(
        *frame, products, frame->metadata.frame.capture_sequence);
    temporary = SealedSharedMemory::create("nuvo-local-frame", parts.wire_bytes());
    wire::write_frame_set_message(parts, temporary.writable_bytes());
    lock.lock();
    if ((latest_ == frame || latest_matcher_ == frame) && parts.wire_bytes() <= kCacheMaximumBytes) {
      while (!cache_.empty() && (cache_.size() >= kCacheMaximumEntries ||
             cache_bytes_ + parts.wire_bytes() > kCacheMaximumBytes)) {
        cache_bytes_ -= cache_.front().memory.size();
        cache_.pop_front();
      }
      cache_bytes_ += parts.wire_bytes();
      cache_.push_back({frame, products, std::move(temporary)});
      memory = &cache_.back().memory;
    } else {
      memory = &temporary;
    }
  }
  std::array<std::byte, 48> response{};
  std::copy(kResponseMagic.begin(), kResponseMagic.end(), response.begin());
  put_u64(response.data() + 8U, memory->size());
  const auto identity = cursor(*frame);
  std::memcpy(response.data() + 16U, identity.run_uuid.data(), 16U);
  put_u64(response.data() + 32U, identity.capture_epoch);
  put_u64(response.data() + 40U, identity.capture_sequence);
  // Duplicate while the cache is protected, then release all producer locks
  // before sending. Publication may now evict the cached owner safely.
#if defined(__linux__)
  OwnedFd exported(::fcntl(memory->readonly_handle(), F_DUPFD_CLOEXEC, 0));
  if (!exported) throw std::runtime_error("duplicate local frame descriptor failed");
  const std::array handle{exported.get()};
#else
  const std::array handle{memory->readonly_handle()};
#endif
  lock.unlock();
  serialization_lock.unlock();
  connection.send_packet(std::span(response).first(wait ? 48U : 16U), handle);
  if (!persistent) return;
  }
}

namespace {
[[nodiscard]] std::optional<MappedLocalFrame> receive_mapped_frame(
    SeqpacketConnection& connection, const bool wait,
    const std::chrono::milliseconds timeout,
    const std::size_t maximum_wire_bytes = wire::kHeaderBytes +
        wire::kHardMaximumMetadataBytes + wire::kHardMaximumPayloadBytes) {
#if defined(__linux__)
  auto response = connection.receive_packet(
      1032U, 1U, timeout);
  if (wait && response.payload.size() == 4U && response.handles.empty() &&
      std::equal(kInvalidatedMagic.begin(), kInvalidatedMagic.end(), response.payload.begin()))
    return MappedLocalFrame{};
  if (wait && response.payload.size() == 4U && response.handles.empty() &&
      std::equal(kNoFrameMagic.begin(), kNoFrameMagic.end(), response.payload.begin())) {
    return std::nullopt;
  }
  if (response.payload.size() >= 4U &&
      std::equal(kErrorMagic.begin(), kErrorMagic.end(),
                 response.payload.begin())) {
    throw std::runtime_error(std::string(
        reinterpret_cast<const char*>(response.payload.data() + 4U),
        response.payload.size() - 4U));
  }
  if (response.payload.size() != (wait ? 48U : 16U) || response.handles.size() != 1U ||
      !std::equal(kResponseMagic.begin(), kResponseMagic.end(),
                  response.payload.begin())) {
    throw std::runtime_error("invalid local frame response");
  }
  const auto size = get_u64(response.payload.data() + 8U);
  if (size == 0U || size > maximum_wire_bytes || size > wire::kHeaderBytes +
                               wire::kHardMaximumMetadataBytes +
                               wire::kHardMaximumPayloadBytes) {
    throw std::length_error("local frame exceeds NuvoStream bounds");
  }
  const auto descriptor = response.handles.front().get();
  struct stat information{};
  const auto seals = ::fcntl(descriptor, F_GET_SEALS);
  if (::fstat(descriptor, &information) != 0 || information.st_size < 0 ||
      static_cast<std::uint64_t>(information.st_size) != size || seals < 0 ||
      (seals & (F_SEAL_GROW | F_SEAL_SHRINK)) != (F_SEAL_GROW | F_SEAL_SHRINK) ||
      (seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) == 0)
    throw std::runtime_error("local frame descriptor lacks sealed immutable extent");
  auto retained = std::make_shared<OwnedFd>(std::move(response.handles.front()));
  auto* mapped = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_PRIVATE, descriptor, 0);
  if (mapped == MAP_FAILED) throw std::runtime_error(std::string("map local frame failed: ") + std::strerror(errno));
  MappedLocalFrame result;
  result.owner = std::shared_ptr<const void>(mapped, [size, retained](const void* address) {
    (void)retained;
    (void)::munmap(const_cast<void*>(address), static_cast<std::size_t>(size));
  });
  if (wait) {
    std::memcpy(result.cursor.run_uuid.data(), response.payload.data() + 16U, 16U);
    result.cursor.capture_epoch = get_u64(response.payload.data() + 32U);
    result.cursor.capture_sequence = get_u64(response.payload.data() + 40U);
  }
  result.bytes = {static_cast<const std::byte*>(mapped), static_cast<std::size_t>(size)};
  return result;
#else
  (void)connection;
  (void)wait;
  (void)timeout;
  throw std::runtime_error("local frame service is available only on Linux");
#endif
}
[[nodiscard]] std::optional<SerializedLocalFrame> receive_frame(
    SeqpacketConnection& connection, bool wait, std::chrono::milliseconds timeout,
    std::size_t maximum_wire_bytes = wire::kHeaderBytes + wire::kHardMaximumMetadataBytes + wire::kHardMaximumPayloadBytes) {
  auto value = receive_mapped_frame(connection, wait, timeout, maximum_wire_bytes);
  if (!value) return {};
  return SerializedLocalFrame{value->cursor, {value->bytes.begin(), value->bytes.end()}};
}
}  // namespace

wire::Message request_latest_local(const std::filesystem::path& path,
                                   const ProductMask products) {
  if (products == 0U || (products & ~kSupportedProducts) != 0U) {
    throw std::invalid_argument("invalid local product mask");
  }
  auto connection = connect_seqpacket(path);
  connection.send_packet(request_packet(products));
  return wire::deserialize(receive_frame(connection, false,
                           std::chrono::seconds(1))->bytes);
}

std::optional<SerializedLocalFrame> request_next_local(
    const std::filesystem::path& path, const ProductMask products,
    const LocalFrameCursor& after, const std::chrono::milliseconds timeout,
    const std::size_t maximum_wire_bytes) {
  if (products == 0U || (products & ~kSupportedProducts) != 0U ||
      timeout.count() < 0 || timeout.count() > 500 ||
      maximum_wire_bytes < wire::kHeaderBytes || maximum_wire_bytes >
        wire::kHeaderBytes + wire::kHardMaximumMetadataBytes + wire::kHardMaximumPayloadBytes) {
    throw std::invalid_argument("invalid local frame subscription bounds");
  }
  std::array<std::byte, 48> request{};
  std::copy(kWaitMagic.begin(), kWaitMagic.end(), request.begin());
  put_u32(request.data() + 4U, products);
  put_u32(request.data() + 8U, static_cast<std::uint32_t>(timeout.count()));
  std::memcpy(request.data() + 16U, after.run_uuid.data(), 16U);
  put_u64(request.data() + 32U, after.capture_epoch);
  put_u64(request.data() + 40U, after.capture_sequence);
  auto connection = connect_seqpacket(path);
  connection.send_packet(request);
  return receive_frame(connection, true, timeout + std::chrono::milliseconds(750),
                       maximum_wire_bytes);
}

LocalFrameClient::LocalFrameClient(const std::filesystem::path& path,
                                  const ProductMask products,
                                  const double cadence_hz,
                                  const double preview_cadence_hz)
    : connection_(connect_seqpacket(path)) {
  set_demand(products, cadence_hz, preview_cadence_hz);
}

void LocalFrameClient::set_demand(const ProductMask products,
                                 const double cadence_hz,
                                 const double preview_cadence_hz) {
  const auto rate_valid = [](const double rate) {
    return std::isfinite(rate) && (rate == 0.0 || (rate >= 0.001 && rate <= 60.0));
  };
  if ((products & ~kSupportedProducts) != 0U || !rate_valid(cadence_hz) ||
      !rate_valid(preview_cadence_hz) || (products == 0U) != (cadence_hz == 0.0))
    throw std::invalid_argument("invalid local demand bounds");
  std::array<std::byte, 20> request{};
  std::copy(kSubscribeMagic.begin(), kSubscribeMagic.end(), request.begin());
  put_u32(request.data() + 4U, products);
  put_u32(request.data() + 8U, static_cast<std::uint32_t>(std::lround(cadence_hz * 1000.0)));
  put_u32(request.data() + 12U, static_cast<std::uint32_t>(std::lround(preview_cadence_hz * 1000.0)));
  connection_.send_packet(request);
  const auto response = connection_.receive_packet(1032U, 0U, std::chrono::seconds(1));
  if (response.payload.size() != kAckMagic.size() ||
      !std::equal(kAckMagic.begin(), kAckMagic.end(), response.payload.begin()))
    throw std::runtime_error("local demand subscription was rejected");
  products_ = products;
}

std::optional<SerializedLocalFrame> LocalFrameClient::next(
    const LocalFrameCursor& after, const std::chrono::milliseconds timeout,
    const std::size_t maximum_wire_bytes) {
  auto value = next_mapped(after, timeout, maximum_wire_bytes);
  if (!value) return {};
  return SerializedLocalFrame{value->cursor, {value->bytes.begin(), value->bytes.end()}};
}

std::optional<MappedLocalFrame> LocalFrameClient::next_mapped(
    const LocalFrameCursor& after, const std::chrono::milliseconds timeout,
    const std::size_t maximum_wire_bytes) {
  if (products_ == 0U) return std::nullopt;
  if (timeout.count() < 0 || timeout.count() > 500 || maximum_wire_bytes < wire::kHeaderBytes ||
      maximum_wire_bytes > wire::kHeaderBytes + wire::kHardMaximumMetadataBytes + wire::kHardMaximumPayloadBytes)
    throw std::invalid_argument("invalid local frame subscription bounds");
  std::array<std::byte, 48> request{};
  std::copy(kWaitMagic.begin(), kWaitMagic.end(), request.begin());
  put_u32(request.data() + 4U, products_);
  put_u32(request.data() + 8U, static_cast<std::uint32_t>(timeout.count()));
  std::memcpy(request.data() + 16U, after.run_uuid.data(), 16U);
  put_u64(request.data() + 32U, after.capture_epoch);
  put_u64(request.data() + 40U, after.capture_sequence);
  connection_.send_packet(request);
  return receive_mapped_frame(connection_, true, timeout + std::chrono::milliseconds(750), maximum_wire_bytes);
}

std::optional<SerializedLocalFrame> LocalFrameClient::next_encoded(
    const LocalFrameCursor& after, const std::chrono::milliseconds timeout,
    const std::size_t maximum_wire_bytes) {
  auto value = next_encoded_mapped(after, timeout, maximum_wire_bytes);
  if (!value) return {};
  return SerializedLocalFrame{value->cursor, {value->bytes.begin(), value->bytes.end()}};
}

std::optional<MappedLocalFrame> LocalFrameClient::next_encoded_mapped(
    const LocalFrameCursor& after, const std::chrono::milliseconds timeout,
    const std::size_t maximum_wire_bytes) {
  if (timeout.count() < 0 || timeout.count() > 500 || maximum_wire_bytes < wire::kHeaderBytes ||
      maximum_wire_bytes > 4U * 1024U * 1024U) throw std::invalid_argument("invalid encoded wait bounds");
  std::array<std::byte, 48> request{};
  std::copy(kEncodedWaitMagic.begin(), kEncodedWaitMagic.end(), request.begin());
  put_u32(request.data() + 8U, static_cast<std::uint32_t>(timeout.count()));
  std::memcpy(request.data() + 16U, after.run_uuid.data(), 16U);
  put_u64(request.data() + 32U, after.capture_epoch);
  put_u64(request.data() + 40U, after.capture_sequence);
  connection_.send_packet(request);
  return receive_mapped_frame(connection_, true, timeout + std::chrono::milliseconds(750), maximum_wire_bytes);
}

}  // namespace depthd::ipc_v2
