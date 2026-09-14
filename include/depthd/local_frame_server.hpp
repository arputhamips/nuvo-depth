#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "depthd/frame_wire.hpp"
#include "depthd/local_ipc.hpp"

namespace depthd::ipc_v2 {

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
  void stop() noexcept;

 private:
  void run() noexcept;
  void serve(SeqpacketConnection connection);

  SeqpacketListener listener_;
  std::mutex mutex_;
  std::shared_ptr<const FrameSetStorage> latest_;
  bool stopping_{};
  std::thread worker_;
};

[[nodiscard]] wire::Message request_latest_local(
    const std::filesystem::path& path = kDataSocketPath,
    ProductMask products = kAllProducts);

}  // namespace depthd::ipc_v2
