#include "depthd/local_ipc.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace depthd::ipc_v2 {
namespace {

#if !defined(__linux__)
[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "Unix SOCK_SEQPACKET IPC is available only on Linux");
}
#endif

#if defined(__linux__)
[[noreturn]] void system_error(const char* operation) {
  throw std::runtime_error(std::string(operation) + ": " +
                           std::strerror(errno));
}

void validate_socket(const OwnedFd& socket) {
  if (!socket) {
    throw std::logic_error("SOCK_SEQPACKET connection is closed");
  }
}
#endif

}  // namespace

OwnedFd::OwnedFd(const int fd) : fd_(fd) {
  if (fd_ < 0) {
    throw std::invalid_argument("owned file descriptor cannot be negative");
  }
}

OwnedFd::~OwnedFd() noexcept { reset(); }
OwnedFd::OwnedFd(OwnedFd&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}
OwnedFd& OwnedFd::operator=(OwnedFd&& other) noexcept {
  if (this != &other) {
    reset();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}
int OwnedFd::get() const noexcept { return fd_; }
OwnedFd::operator bool() const noexcept { return fd_ >= 0; }
int OwnedFd::release() noexcept { return std::exchange(fd_, -1); }
void OwnedFd::reset() noexcept {
  const auto fd = std::exchange(fd_, -1);
#if defined(__linux__)
  if (fd >= 0) {
    (void)::close(fd);
  }
#else
  (void)fd;
#endif
}

SealedSharedMemory::~SealedSharedMemory() noexcept { reset(); }
SealedSharedMemory::SealedSharedMemory(SealedSharedMemory&& other) noexcept
    : writer_(std::move(other.writer_)),
      readonly_(std::move(other.readonly_)),
      mapping_(std::exchange(other.mapping_, nullptr)),
      bytes_(std::exchange(other.bytes_, 0U)) {}
SealedSharedMemory& SealedSharedMemory::operator=(
    SealedSharedMemory&& other) noexcept {
  if (this != &other) {
    reset();
    writer_ = std::move(other.writer_);
    readonly_ = std::move(other.readonly_);
    mapping_ = std::exchange(other.mapping_, nullptr);
    bytes_ = std::exchange(other.bytes_, 0U);
  }
  return *this;
}

SealedSharedMemory SealedSharedMemory::create(const char* name,
                                               const std::size_t bytes) {
#if defined(__linux__)
  if (name == nullptr || *name == '\0' || bytes == 0U ||
      bytes > 65U * 1024U * 1024U) {
    throw std::invalid_argument("sealed shared-memory settings are invalid");
  }
  SealedSharedMemory result;
  result.writer_ =
      OwnedFd(::memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (::ftruncate(result.writer_.get(), static_cast<off_t>(bytes)) != 0) {
    system_error("size sealed shared memory");
  }
  result.mapping_ = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                           result.writer_.get(), 0);
  if (result.mapping_ == MAP_FAILED) {
    result.mapping_ = nullptr;
    system_error("map sealed shared memory writer");
  }
  result.bytes_ = bytes;
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif
  constexpr int seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE |
                        F_SEAL_SEAL;
  if (::fcntl(result.writer_.get(), F_ADD_SEALS, seals) != 0) {
    system_error("seal shared memory");
  }
  const auto descriptor_path =
      std::string("/proc/self/fd/") + std::to_string(result.writer_.get());
  result.readonly_ = OwnedFd(::open(descriptor_path.c_str(), O_RDONLY | O_CLOEXEC));
  return result;
#else
  (void)name;
  (void)bytes;
  unavailable();
#endif
}

std::span<std::byte> SealedSharedMemory::writable_bytes() noexcept {
  return {static_cast<std::byte*>(mapping_), bytes_};
}
int SealedSharedMemory::readonly_handle() const noexcept {
  return readonly_.get();
}
std::size_t SealedSharedMemory::size() const noexcept { return bytes_; }
void SealedSharedMemory::reset() noexcept {
#if defined(__linux__)
  if (mapping_ != nullptr) {
    (void)::munmap(mapping_, bytes_);
  }
#endif
  mapping_ = nullptr;
  bytes_ = 0U;
  readonly_.reset();
  writer_.reset();
}

SeqpacketConnection::SeqpacketConnection(OwnedFd socket)
    : socket_(std::move(socket)) {}

SeqpacketConnection::operator bool() const noexcept {
  return static_cast<bool>(socket_);
}

int SeqpacketConnection::native_handle() const noexcept { return socket_.get(); }

PeerCredentials SeqpacketConnection::peer_credentials() const {
#if defined(__linux__)
  validate_socket(socket_);
  struct ucred credentials {};
  socklen_t size = sizeof(credentials);
  if (::getsockopt(socket_.get(), SOL_SOCKET, SO_PEERCRED, &credentials,
                   &size) != 0 || size != sizeof(credentials)) {
    system_error("SO_PEERCRED");
  }
  return {credentials.pid, credentials.uid, credentials.gid};
#else
  unavailable();
#endif
}

void SeqpacketConnection::send_packet(
    const std::span<const std::byte> payload,
    const std::span<const int> handles) const {
#if defined(__linux__)
  validate_socket(socket_);
  if (payload.empty() || handles.size() > 16U) {
    throw std::invalid_argument(
        "SOCK_SEQPACKET payload must be nonempty and carry at most 16 handles");
  }
  for (const auto fd : handles) {
    if (fd < 0) {
      throw std::invalid_argument("cannot transfer an invalid file descriptor");
    }
  }
  struct iovec vector {
    const_cast<std::byte*>(payload.data()), payload.size()
  };
  struct msghdr message {};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  std::vector<unsigned char> control;
  if (!handles.empty()) {
    control.resize(CMSG_SPACE(handles.size() * sizeof(int)));
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(handles.size() * sizeof(int));
    std::memcpy(CMSG_DATA(header), handles.data(),
                handles.size() * sizeof(int));
  }
  const auto sent = ::sendmsg(socket_.get(), &message, MSG_NOSIGNAL);
  if (sent < 0) {
    system_error("sendmsg(SOCK_SEQPACKET)");
  }
  if (static_cast<std::size_t>(sent) != payload.size()) {
    throw std::runtime_error("SOCK_SEQPACKET send was unexpectedly partial");
  }
#else
  (void)payload;
  (void)handles;
  unavailable();
#endif
}

ReceivedPacket SeqpacketConnection::receive_packet(
    const std::size_t maximum_payload_bytes,
    const std::size_t maximum_handles,
    const std::chrono::milliseconds timeout) const {
#if defined(__linux__)
  validate_socket(socket_);
  if (maximum_payload_bytes == 0U || maximum_payload_bytes > 64U * 1024U * 1024U ||
      maximum_handles > 16U || timeout.count() < -1 ||
      timeout.count() > 300'000) {
    throw std::invalid_argument("SOCK_SEQPACKET receive bounds are invalid");
  }
  if (timeout.count() >= 0) {
    struct pollfd descriptor { socket_.get(), POLLIN, 0 };
    int ready{};
    do {
      ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) {
      system_error("poll SOCK_SEQPACKET connection");
    }
    if (ready == 0) {
      throw std::runtime_error("SOCK_SEQPACKET receive timed out");
    }
  }
  ReceivedPacket result;
  result.payload.resize(maximum_payload_bytes);
  struct iovec vector { result.payload.data(), result.payload.size() };
  std::vector<unsigned char> control(
      maximum_handles == 0U ? 1U
                            : CMSG_SPACE(maximum_handles * sizeof(int)));
  struct msghdr message {};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  if (maximum_handles != 0U) {
    message.msg_control = control.data();
    message.msg_controllen = control.size();
  }
  const auto received = ::recvmsg(socket_.get(), &message, MSG_CMSG_CLOEXEC);
  if (received < 0) {
    system_error("recvmsg(SOCK_SEQPACKET)");
  }
  if (received == 0) {
    throw std::runtime_error("SOCK_SEQPACKET peer disconnected");
  }
  // Adopt every complete received descriptor before any later validation can
  // throw. This guarantees malformed/truncated ancillary data cannot leak FDs.
  for (auto* header = CMSG_FIRSTHDR(&message); header != nullptr;
       header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len < CMSG_LEN(0)) {
      continue;
    }
    const auto payload_bytes = header->cmsg_len - CMSG_LEN(0);
    const auto count = payload_bytes / sizeof(int);
    const auto* descriptors = reinterpret_cast<const int*>(CMSG_DATA(header));
    for (std::size_t index = 0; index < count; ++index) {
      result.handles.emplace_back(descriptors[index]);
    }
    if (payload_bytes % sizeof(int) != 0U) {
      throw std::runtime_error("malformed SCM_RIGHTS control message");
    }
  }
  if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
    throw std::length_error("SOCK_SEQPACKET message exceeded receive bounds");
  }
  if (result.handles.size() > maximum_handles) {
    throw std::length_error("SCM_RIGHTS handle count exceeded receive bound");
  }
  result.payload.resize(static_cast<std::size_t>(received));
  return result;
#else
  (void)maximum_payload_bytes;
  (void)maximum_handles;
  (void)timeout;
  unavailable();
#endif
}

SeqpacketListener::SeqpacketListener(const std::filesystem::path& path,
                                     const std::uint16_t permissions,
                                     const int backlog)
    : path_(path) {
#if defined(__linux__)
  const auto native = path_.string();
  if (!path_.is_absolute() || native.empty() ||
      native.size() >= sizeof(sockaddr_un{}.sun_path) ||
      (permissions & ~0777U) != 0U || backlog < 1 || backlog > 128) {
    throw std::invalid_argument("SOCK_SEQPACKET listener settings are invalid");
  }
  struct stat existing {};
  if (::lstat(native.c_str(), &existing) == 0) {
    if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid()) {
      throw std::runtime_error(
          "refusing to replace a non-socket or foreign local IPC path");
    }
    if (::unlink(native.c_str()) != 0) {
      system_error("unlink stale local IPC socket");
    }
  } else if (errno != ENOENT) {
    system_error("inspect local IPC socket path");
  }

  OwnedFd candidate(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  // A full peer backlog or a non-reading peer must not indefinitely block a
  // producer/gateway shutdown. Packet receives have their own explicit bound.
  const timeval timeout{1, 0};
  if (::setsockopt(candidate.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
    system_error("set local IPC send/connect deadline");
  }
  struct sockaddr_un address {};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, native.c_str(), native.size() + 1U);
  if (::bind(candidate.get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
    system_error("bind local IPC socket");
  }
  if (::chmod(native.c_str(), permissions) != 0) {
    (void)::unlink(native.c_str());
    system_error("chmod local IPC socket");
  }
  if (::listen(candidate.get(), backlog) != 0) {
    (void)::unlink(native.c_str());
    system_error("listen local IPC socket");
  }
  struct stat bound {};
  if (::lstat(native.c_str(), &bound) != 0) {
    (void)::unlink(native.c_str());
    system_error("stat bound local IPC socket");
  }
  bound_device_ = static_cast<std::uint64_t>(bound.st_dev);
  bound_inode_ = static_cast<std::uint64_t>(bound.st_ino);
  socket_ = std::move(candidate);
#else
  (void)permissions;
  (void)backlog;
  unavailable();
#endif
}

SeqpacketListener::~SeqpacketListener() noexcept {
#if defined(__linux__)
  socket_.reset();
  const auto native = path_.string();
  struct stat current {};
  if (::lstat(native.c_str(), &current) == 0 && S_ISSOCK(current.st_mode) &&
      static_cast<std::uint64_t>(current.st_dev) == bound_device_ &&
      static_cast<std::uint64_t>(current.st_ino) == bound_inode_) {
    (void)::unlink(native.c_str());
  }
#endif
}

SeqpacketConnection SeqpacketListener::accept(
    const std::chrono::milliseconds timeout) const {
#if defined(__linux__)
  validate_socket(socket_);
  if (timeout.count() < 0 || timeout.count() > 300'000) {
    throw std::invalid_argument("SOCK_SEQPACKET accept timeout is invalid");
  }
  struct pollfd descriptor { socket_.get(), POLLIN, 0 };
  const auto ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
  if (ready < 0) {
    system_error("poll local IPC listener");
  }
  if (ready == 0) {
    return {};
  }
  const auto accepted = ::accept4(socket_.get(), nullptr, nullptr, SOCK_CLOEXEC);
  if (accepted < 0) {
    system_error("accept local IPC connection");
  }
  return SeqpacketConnection(OwnedFd(accepted));
#else
  (void)timeout;
  unavailable();
#endif
}

const std::filesystem::path& SeqpacketListener::path() const noexcept {
  return path_;
}

SeqpacketConnection connect_seqpacket(const std::filesystem::path& path) {
#if defined(__linux__)
  const auto native = path.string();
  if (!path.is_absolute() || native.empty() ||
      native.size() >= sizeof(sockaddr_un{}.sun_path)) {
    throw std::invalid_argument("SOCK_SEQPACKET connection path is invalid");
  }
  OwnedFd candidate(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  const timeval timeout{1, 0};
  if (::setsockopt(candidate.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
    system_error("set local IPC connect/send deadline");
  }
  struct sockaddr_un address {};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, native.c_str(), native.size() + 1U);
  if (::connect(candidate.get(), reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
    system_error("connect local IPC socket");
  }
  return SeqpacketConnection(std::move(candidate));
#else
  (void)path;
  unavailable();
#endif
}

bool seqpacket_transport_available() noexcept {
#if defined(__linux__)
  return true;
#else
  return false;
#endif
}

}  // namespace depthd::ipc_v2
