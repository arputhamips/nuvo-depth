#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>

#include "nuvo/client.hpp"
#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#endif

namespace nuvo::client {
#if defined(__linux__)
namespace {
struct Fd {
  int value{-1};
  explicit Fd(int fd = -1) : value(fd) {}
  ~Fd() {
    if (value >= 0) ::close(value);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};
void put(std::byte* p, std::uint64_t v, unsigned n) {
  for (unsigned i = 0; i < n; ++i)
    p[i] = std::byte((v >> (8U * (n - i - 1))) & 255U);
}
std::uint64_t get(const std::byte* p, unsigned n) {
  std::uint64_t v{};
  for (unsigned i = 0; i < n; ++i)
    v = (v << 8U) | std::to_integer<unsigned>(p[i]);
  return v;
}
std::uint32_t mask(const Subscription& s) {
  constexpr std::array<std::string_view, 14> names{
      "left_rectified",     "right_rectified",
      "disparity",          "depth",
      "confidence",         "validity",
      "left_mono",          "right_mono",
      "left_rgb",           "right_rgb",
      "left_rectified_rgb", "right_rectified_rgb",
      "rectified_stereo",   "rectified_stereo_rgb"};
  std::uint32_t value{};
  for (const auto& p : s.products)
    for (unsigned i = 0; i < names.size(); ++i)
      if (p == names[i]) value |= 1U << i;
  return value;
}
class Local final : public Source {
 public:
  Local(const std::filesystem::path& path, const Subscription& subscription)
      : products_(mask(subscription)) {
    validate(subscription);
    if (subscription.delivery == "exact")
      throw std::invalid_argument(
          "local IPC cadence is best effort; exact admission belongs to HTTPS");
    const auto name = path.string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (name.empty() || name.size() >= sizeof(address.sun_path))
      throw std::invalid_argument("invalid local socket path");
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    socket_ = std::make_unique<Fd>(
        ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (socket_->value < 0)
      throw std::runtime_error("cannot create Nuvo data socket");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (::connect(socket_->value, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address)) != 0) {
      if (errno == EISCONN) break;
      if ((errno != EAGAIN && errno != EINTR && errno != EINPROGRESS) ||
          std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error(
            "cannot connect Nuvo data socket within deadline; check service, "
            "mount, backlog and nuvo-video group");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto flags = ::fcntl(socket_->value, F_GETFL);
    if (flags < 0 || ::fcntl(socket_->value, F_SETFL, flags & ~O_NONBLOCK) != 0)
      throw std::runtime_error("cannot configure Nuvo data socket");
    timeval timeout{1, 0};
    ::setsockopt(socket_->value, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout));
    std::array<std::byte, 20> request{};
    std::memcpy(request.data(), "NVS2", 4);
    put(request.data() + 4, products_, 4);
    put(request.data() + 8,
        static_cast<std::uint64_t>(subscription.cadence_hz * 1000 + .5), 4);
    send(request);
    auto ack = receive();
    if (ack.size() != 4 || std::memcmp(ack.data(), "NVA2", 4) != 0)
      throw std::runtime_error("local subscription rejected");
  }
  ~Local() override { cancel(); }
  void cancel() noexcept override {
    stopped_ = true;
    if (socket_) ::shutdown(socket_->value, SHUT_RDWR);
  }
  std::optional<Frame> next() override {
    if (stopped_) return {};
    try {
      return acquire();
    } catch (...) {
      // Never issue another request after a partial response or invalid frame.
      cancel();
      throw;
    }
  }

 private:
  std::optional<Frame> acquire() {
    std::array<std::byte, 48> request{};
    std::memcpy(request.data(), "NVW2", 4);
    put(request.data() + 4, products_, 4);
    put(request.data() + 8, 400, 4);
    std::memcpy(request.data() + 16, cursor_.data(), 32);
    send(request);
    std::unique_ptr<Fd> handle;
    auto response = receive(&handle);
    if (stopped_) return {};
    if (response.size() == 4 && std::memcmp(response.data(), "NVN2", 4) == 0 &&
        !handle)
      return {};
    if (response.size() == 4 && std::memcmp(response.data(), "NVI2", 4) == 0 &&
        !handle)
      throw SourceInvalidated(
          "Nuvo local publication invalidated; renegotiate subscription");
    if (response.size() != 48 || std::memcmp(response.data(), "NVR2", 4) != 0 ||
        !handle)
      throw std::runtime_error("invalid local frame response");
    const auto size = get(response.data() + 8, 8);
    if (size < 32 || size > 16U * 1024U * 1024U)
      throw std::runtime_error("local frame exceeds size bound");
    struct stat status {};
    const auto seals = ::fcntl(handle->value, F_GET_SEALS);
    if (::fstat(handle->value, &status) != 0 ||
        status.st_size != static_cast<off_t>(size) || seals < 0 ||
        (seals & (F_SEAL_GROW | F_SEAL_SHRINK)) !=
            (F_SEAL_GROW | F_SEAL_SHRINK) ||
        (seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) == 0 ||
        (::fcntl(handle->value, F_GETFL) & O_ACCMODE) != O_RDONLY)
      throw std::runtime_error(
          "local descriptor must be read-only, size sealed and write sealed");
    void* mapped =
        ::mmap(nullptr, size, PROT_READ, MAP_SHARED, handle->value, 0);
    if (mapped == MAP_FAILED)
      throw std::runtime_error("local frame mmap failed");
    // mmap keeps the storage alive after the descriptor closes. Retained
    // frames therefore consume mappings, not one process descriptor each.
    handle.reset();
    auto owner =
        std::shared_ptr<const void>(mapped, [size](const void* p) {
          ::munmap(const_cast<void*>(p), size);
        });
    auto frame = decode_frame(
        {static_cast<const std::byte*>(mapped), static_cast<std::size_t>(size)},
        owner);
    constexpr char digits[] = "0123456789abcdef";
    std::string run;
    for (unsigned i = 16; i < 32; ++i) {
      const auto b = std::to_integer<unsigned>(response[i]);
      run += digits[b >> 4U];
      run += digits[b & 15U];
    }
    if (run != frame.identity.run_uuid ||
        get(response.data() + 32, 8) != frame.identity.capture_epoch ||
        get(response.data() + 40, 8) != frame.identity.capture_sequence)
      throw std::runtime_error("local cursor differs from frame identity");
    std::memcpy(cursor_.data(), response.data() + 16, 32);
    return frame;
  }

  void send(std::span<const std::byte> bytes) {
    if (::send(socket_->value, bytes.data(), bytes.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(bytes.size()))
      throw std::runtime_error("local send failed or cancelled");
  }
  std::vector<std::byte> receive(std::unique_ptr<Fd>* output = nullptr) {
    pollfd descriptor{socket_->value, POLLIN, 0};
    int ready;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    do {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now())
              .count();
      if (remaining <= 0) throw std::runtime_error("local response timed out");
      ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
    } while (ready < 0 && errno == EINTR && !stopped_);
    if (ready <= 0)
      throw std::runtime_error("local response timed out or cancelled");
    std::array<std::byte, 1032> payload{};
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(4 * sizeof(int))>
        ancillary{};
    iovec vector{payload.data(), payload.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = ancillary.data();
    message.msg_controllen = ancillary.size();
    const auto count = ::recvmsg(socket_->value, &message, MSG_CMSG_CLOEXEC);
    std::vector<std::unique_ptr<Fd>> handles;
    for (auto* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
          c->cmsg_len >= CMSG_LEN(0)) {
        const auto number = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        auto* fds = reinterpret_cast<int*>(CMSG_DATA(c));
        for (std::size_t i = 0; i < number; ++i)
          handles.push_back(std::make_unique<Fd>(fds[i]));
      }
    }
    if (count <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        handles.size() > (output ? 1U : 0U))
      throw std::runtime_error(
          "local connection closed or malformed ancillary response");
    if (output && !handles.empty()) *output = std::move(handles.front());
    if (count >= 4 && std::memcmp(payload.data(), "NVE2", 4) == 0)
      throw std::runtime_error("Nuvo local server rejected request");
    return {payload.begin(), payload.begin() + count};
  }
  std::unique_ptr<Fd> socket_;
  std::uint32_t products_;
  std::array<std::byte, 32> cursor_{};
  std::atomic_bool stopped_{};
};
std::int64_t ns(clockid_t clock) {
  timespec value{};
  if (::clock_gettime(clock, &value) != 0)
    throw std::runtime_error("clock unavailable");
  return value.tv_sec * 1000000000LL + value.tv_nsec;
}
}  // namespace
std::unique_ptr<Source> local_source(const std::filesystem::path& path,
                                     Subscription s) {
  return std::make_unique<Local>(path, s);
}
ClockSample local_clock_sample() {
  // The appliance is native in the initial time namespace. A container with
  // explicit boottime offsets cannot use the direct same-host mapping.
  static const bool checked = [] {
    std::ifstream input("/proc/self/timens_offsets");
    std::string name;
    long long seconds{}, nanoseconds{};
    while (input >> name >> seconds >> nanoseconds)
      if (name == "boottime" && (seconds != 0 || nanoseconds != 0))
        throw std::runtime_error(
            "local clock correlation requires the native boottime namespace");
    return true;
  }();
  (void)checked;
  const auto before = ns(CLOCK_BOOTTIME), system = ns(CLOCK_REALTIME),
             after = ns(CLOCK_BOOTTIME);
  return {before + (after - before) / 2, system, (after - before + 1) / 2};
}
#else
std::unique_ptr<Source> local_source(const std::filesystem::path&,
                                     Subscription) {
  throw std::runtime_error("local IPC requires Linux");
}
ClockSample local_clock_sample() {
  throw std::runtime_error("local clock correlation requires Linux");
}
#endif
}  // namespace nuvo::client
