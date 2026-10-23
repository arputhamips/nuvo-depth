#pragma once

#include <filesystem>
#include <condition_variable>
#include <deque>
#include <optional>
#include <memory>
#include <mutex>
#include <thread>
#include <map>

#include "depthd/frame_wire.hpp"
#include "depthd/local_ipc.hpp"

namespace depthd::ipc_v2 {

struct LocalFrameCursor {
  std::array<std::uint8_t, 16> run_uuid{};
  std::uint64_t capture_epoch{};
  std::uint64_t capture_sequence{};
  bool operator==(const LocalFrameCursor&) const = default;
};

struct SerializedLocalFrame {
  LocalFrameCursor cursor;
  std::vector<std::byte> bytes;
};

struct MappedLocalFrame {
  LocalFrameCursor cursor;
  std::shared_ptr<const void> owner;
  std::span<const std::byte> bytes;
};

struct LocalFrameDemand {
  ProductMask products{};
  double cadence_hz{};
  double preview_cadence_hz{};
  std::uint64_t generation{1U};
  // Keep associated masks intact. Image-only clients must not raise the
  // cadence of clients requiring the stateful stereo matcher.
  ProductMask image_products{};
  double image_cadence_hz{};
  ProductMask matcher_products{};
  double matcher_cadence_hz{};
};

// One sequential client can keep its demand registered while repeatedly
// waiting for sealed frame messages. Destroy/disconnect releases the demand.
class LocalFrameClient final {
 public:
  explicit LocalFrameClient(const std::filesystem::path& path = kDataSocketPath,
                            ProductMask products = kAllProducts,
                            double cadence_hz = 10.0,
                            double preview_cadence_hz = 0.0);
  void set_demand(ProductMask products, double cadence_hz,
                  double preview_cadence_hz = 0.0);
  [[nodiscard]] std::optional<SerializedLocalFrame> next(
      const LocalFrameCursor& after = {},
      std::chrono::milliseconds timeout = std::chrono::milliseconds(400),
      std::size_t maximum_wire_bytes = wire::kHeaderBytes +
          wire::kHardMaximumMetadataBytes + wire::kHardMaximumPayloadBytes);
  [[nodiscard]] std::optional<SerializedLocalFrame> next_encoded(
      const LocalFrameCursor& after = {},
      std::chrono::milliseconds timeout = std::chrono::milliseconds(400),
      std::size_t maximum_wire_bytes = 4U * 1024U * 1024U);
  [[nodiscard]] std::optional<MappedLocalFrame> next_mapped(
      const LocalFrameCursor& after = {},
      std::chrono::milliseconds timeout = std::chrono::milliseconds(400),
      std::size_t maximum_wire_bytes = 16U * 1024U * 1024U);
  [[nodiscard]] std::optional<MappedLocalFrame> next_encoded_mapped(
      const LocalFrameCursor& after = {},
      std::chrono::milliseconds timeout = std::chrono::milliseconds(400),
      std::size_t maximum_wire_bytes = 4U * 1024U * 1024U);
 private:
  SeqpacketConnection connection_;
  ProductMask products_{};
};

// Copy-safe local frame service. A client requests a product mask over a
// restricted SOCK_SEQPACKET socket and receives one sealed, read-only memfd
// containing a complete NuvoStream message. The publication pointer is swapped
// atomically at frame-set granularity; products can never come from two frames.
class LocalFrameServer final {
 public:
  explicit LocalFrameServer(
      std::filesystem::path path = kDataSocketPath,
      std::uint16_t permissions = 0660U);
  ~LocalFrameServer() noexcept;
  LocalFrameServer(const LocalFrameServer&) = delete;
  LocalFrameServer& operator=(const LocalFrameServer&) = delete;

  void publish(std::shared_ptr<const FrameSetStorage> frame);
  // Empty bytes invalidate the current preview after demand/configuration changes.
  void publish_encoded(const LocalFrameCursor& cursor, std::span<const std::byte> bytes);
  [[nodiscard]] LocalFrameDemand demand() const noexcept;
  void stop() noexcept;

 private:
  void run() noexcept;
  void serve(SeqpacketConnection connection);
  void recompute_demand_locked() noexcept;

  SeqpacketListener listener_;
  mutable std::mutex mutex_;
  std::mutex serialization_mutex_;
  std::condition_variable changed_;
  std::shared_ptr<const FrameSetStorage> latest_;
  std::shared_ptr<const FrameSetStorage> latest_matcher_;
  struct EncodedPublication { LocalFrameCursor cursor; SealedSharedMemory memory; };
  std::shared_ptr<const EncodedPublication> encoded_;
  struct CachedFrame {
    // Exact immutable publication plus selection; weak ownership avoids
    // pinning a processing pool slot just to retain serialized bytes.
    std::weak_ptr<const FrameSetStorage> frame;
    ProductMask products;
    SealedSharedMemory memory;
  };
  std::deque<CachedFrame> cache_;
  std::size_t cache_bytes_{};
  struct ClientDemand {
    int socket{-1};
    ProductMask products{};
    double cadence_hz{};
    double preview_cadence_hz{};
    bool waiting{};
  };
  std::map<std::uint64_t, ClientDemand> clients_;
  std::uint64_t next_client_id_{1U};
  LocalFrameDemand demand_{};
  bool stopping_{};
  std::thread worker_;
};

[[nodiscard]] wire::Message request_latest_local(
    const std::filesystem::path& path = kDataSocketPath,
    ProductMask products = kAllProducts);

// Waits at most timeout for a different complete frame identity. A timeout
// returns no value. Serialized bytes are copied once from the sealed memfd;
// gateways can share that immutable buffer across snapshot and stream clients.
[[nodiscard]] std::optional<SerializedLocalFrame> request_next_local(
    const std::filesystem::path& path, ProductMask products,
    const LocalFrameCursor& after = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(400),
    std::size_t maximum_wire_bytes = wire::kHeaderBytes +
        wire::kHardMaximumMetadataBytes + wire::kHardMaximumPayloadBytes);

}  // namespace depthd::ipc_v2
