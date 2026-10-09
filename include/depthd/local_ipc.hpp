#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace depthd::ipc_v2 {

inline constexpr const char* kControlSocketPath =
    "/run/nuvo-depth/control.sock";
inline constexpr const char* kDataSocketPath = "/run/nuvo-depth/data.sock";

struct PeerCredentials {
  std::int64_t process_id{-1};
  std::uint32_t user_id{};
  std::uint32_t group_id{};
};

class OwnedFd final {
 public:
  OwnedFd() = default;
  explicit OwnedFd(int fd);
  ~OwnedFd() noexcept;
  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;
  OwnedFd(OwnedFd&&) noexcept;
  OwnedFd& operator=(OwnedFd&&) noexcept;

  [[nodiscard]] int get() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] int release() noexcept;
  void reset() noexcept;

 private:
  int fd_{-1};
};

// Linux memfd storage with a writer mapping created before
// F_SEAL_FUTURE_WRITE and a separate read-only descriptor for SCM_RIGHTS.
// Existing producer writes remain possible; receivers cannot create writable
// mappings or issue write syscalls through the exported handle.
class SealedSharedMemory final {
 public:
  SealedSharedMemory() = default;
  ~SealedSharedMemory() noexcept;
  SealedSharedMemory(const SealedSharedMemory&) = delete;
  SealedSharedMemory& operator=(const SealedSharedMemory&) = delete;
  SealedSharedMemory(SealedSharedMemory&&) noexcept;
  SealedSharedMemory& operator=(SealedSharedMemory&&) noexcept;

  [[nodiscard]] static SealedSharedMemory create(const char* name,
                                                  std::size_t bytes);
  [[nodiscard]] std::span<std::byte> writable_bytes() noexcept;
  [[nodiscard]] int readonly_handle() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

 private:
  void reset() noexcept;
  OwnedFd writer_;
  OwnedFd readonly_;
  void* mapping_{};
  std::size_t bytes_{};
};

struct ReceivedPacket {
  std::vector<std::byte> payload;
  std::vector<OwnedFd> handles;
};

class SeqpacketConnection final {
 public:
  SeqpacketConnection() = default;
  explicit SeqpacketConnection(OwnedFd socket);
  SeqpacketConnection(const SeqpacketConnection&) = delete;
  SeqpacketConnection& operator=(const SeqpacketConnection&) = delete;
  SeqpacketConnection(SeqpacketConnection&&) noexcept = default;
  SeqpacketConnection& operator=(SeqpacketConnection&&) noexcept = default;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] PeerCredentials peer_credentials() const;
  [[nodiscard]] int native_handle() const noexcept;
  void send_packet(std::span<const std::byte> payload,
                   std::span<const int> handles = {}) const;
  [[nodiscard]] ReceivedPacket receive_packet(
      std::size_t maximum_payload_bytes,
      std::size_t maximum_handles = 16U,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{-1}) const;

 private:
  OwnedFd socket_;
};

class SeqpacketListener final {
 public:
  SeqpacketListener(const std::filesystem::path& path,
                    std::uint16_t permissions = 0660U,
                    int backlog = 16);
  ~SeqpacketListener() noexcept;
  SeqpacketListener(const SeqpacketListener&) = delete;
  SeqpacketListener& operator=(const SeqpacketListener&) = delete;
  SeqpacketListener(SeqpacketListener&&) = delete;
  SeqpacketListener& operator=(SeqpacketListener&&) = delete;

  [[nodiscard]] SeqpacketConnection accept(
      std::chrono::milliseconds timeout) const;
  [[nodiscard]] const std::filesystem::path& path() const noexcept;

 private:
  std::filesystem::path path_;
  OwnedFd socket_;
  std::uint64_t bound_device_{};
  std::uint64_t bound_inode_{};
};

[[nodiscard]] SeqpacketConnection connect_seqpacket(
    const std::filesystem::path& path);
[[nodiscard]] bool seqpacket_transport_available() noexcept;

}  // namespace depthd::ipc_v2
