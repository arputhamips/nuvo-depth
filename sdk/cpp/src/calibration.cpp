#include "nuvo/client.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>
#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace nuvo::client {
#if defined(__linux__)
namespace {
constexpr std::size_t file_limit = 4U * 1024U * 1024U;
constexpr std::size_t upload_limit = 4U * file_limit + 65536U;
constexpr std::array<std::string_view, 4> assets{
    "manifest.json", "front_left.yaml", "front_right.yaml", "validation.json"};
using Deadline = std::chrono::steady_clock::time_point;
struct Fd {
  int value;
  explicit Fd(int descriptor) : value(descriptor) {}
  ~Fd() { if (value >= 0) ::close(value); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};
[[noreturn]] void timeout_error() {
  throw std::runtime_error(
      "calibration confirmation timed out; the native transaction may finish "
      "independently. Inspect nuvoctl calibration status on the host, then retry "
      "the same bundle to verify completion");
}
void wait(int fd, short events, Deadline deadline) {
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) timeout_error();
    pollfd descriptor{fd, events, 0};
    const auto result = ::poll(&descriptor, 1, static_cast<int>(remaining));
    if (result > 0) return;
    if (result == 0) timeout_error();
    if (errno != EINTR) throw std::runtime_error("calibration socket poll failed");
  }
}
void transfer(int fd, std::span<std::byte> data, bool sending, Deadline deadline) {
  while (!data.empty()) {
    wait(fd, sending ? POLLOUT : POLLIN, deadline);
    const auto count = sending
        ? ::send(fd, data.data(), data.size(), MSG_NOSIGNAL)
        : ::recv(fd, data.data(), data.size(), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0)
      throw std::runtime_error(
          "calibration connection closed before confirmation; inspect native "
          "nuvoctl calibration status and retry the same bundle");
    data = data.subspan(static_cast<std::size_t>(count));
  }
}
std::vector<std::byte> read_file(const std::filesystem::path& path, std::size_t limit) {
  Fd input(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  struct stat status {};
  if (input.value < 0 || ::fstat(input.value, &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) > limit)
    throw std::invalid_argument(
        "calibration input must be bounded regular files, without links; "
        "use a complete accepted native bundle directory or uncompressed tar");
  std::vector<std::byte> data(static_cast<std::size_t>(status.st_size));
  std::size_t offset{};
  while (offset < data.size()) {
    const auto count = ::read(input.value, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("calibration input changed or could not be read");
    offset += static_cast<std::size_t>(count);
  }
  std::byte extra{};
  if (::read(input.value, &extra, 1) != 0)
    throw std::runtime_error("calibration input changed while being read");
  return data;
}
void octal(std::byte* field, std::size_t width, std::size_t value) {
  std::fill(field, field + width, std::byte{'0'});
  field[width - 1] = std::byte{0};
  for (std::size_t i = width - 1; i > 0 && value; value >>= 3U)
    field[--i] = std::byte('0' + (value & 7U));
}
std::vector<std::byte> archive(const std::filesystem::path& path) {
  const auto kind = std::filesystem::symlink_status(path).type();
  if (kind == std::filesystem::file_type::regular) {
    auto data = read_file(path, upload_limit);
    if (data.size() < 1024 || data.size() % 512 != 0)
      throw std::invalid_argument(
          "calibration_file needs a complete accepted native bundle directory "
          "or uncompressed .tar; a single-camera ROS YAML is insufficient");
    return data;
  }
  if (kind != std::filesystem::file_type::directory)
    throw std::invalid_argument("calibration bundle must be a directory or uncompressed tar, without links");
  std::vector<std::byte> data;
  for (const auto name : assets) {
    auto content = read_file(path / name, file_limit);
    const auto start = data.size();
    data.resize(start + 512);
    auto* header = data.data() + start;
    std::memcpy(header, name.data(), name.size());
    octal(header + 100, 8, 0644);
    octal(header + 108, 8, 0);
    octal(header + 116, 8, 0);
    octal(header + 124, 12, content.size());
    octal(header + 136, 12, 0);
    std::fill(header + 148, header + 156, std::byte{' '});
    header[156] = std::byte{'0'};
    std::memcpy(header + 257, "ustar", 5);
    std::memcpy(header + 263, "00", 2);
    std::size_t sum{};
    for (std::size_t i = 0; i < 512; ++i) sum += std::to_integer<unsigned>(header[i]);
    octal(header + 148, 7, sum);
    header[155] = std::byte{' '};
    data.insert(data.end(), content.begin(), content.end());
    data.resize((data.size() + 511U) / 512U * 512U);
  }
  data.resize(data.size() + 1024U);
  return data;
}
void put(std::byte* p, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    p[i] = std::byte((value >> (8U * (width - i - 1))) & 255U);
}
std::uint32_t get(const std::byte* p) {
  std::uint32_t value{};
  for (unsigned i = 0; i < 4; ++i) value = (value << 8U) | std::to_integer<unsigned>(p[i]);
  return value;
}
bool hex(std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
CalibrationActivation decode(std::span<const std::byte> data) {
  using Json = nlohmann::json;
  std::set<std::string> keys;
  const auto response = Json::parse(data.begin(), data.end(),
      [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 2) throw std::runtime_error("calibration reply nesting exceeds bound");
        if (event == Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
          throw std::runtime_error("duplicate calibration reply field");
        return true;
      });
  if (!response.is_object() || !response.at("schema_version").is_number_unsigned() ||
      response.at("schema_version") != 1 ||
      !response.at("ok").is_boolean() || !response.at("code").is_string())
    throw std::runtime_error("unsupported calibration control reply");
  const auto code = response.at("code").get<std::string>();
  if (!response.at("ok").get<bool>()) {
    const auto message = response.at("message").get<std::string>();
    if (code.size() > 64 || message.size() > 2048)
      throw std::runtime_error("calibration control error exceeds bounds");
    throw std::runtime_error("native calibration " + code + ": " + message);
  }
  CalibrationActivation result;
  result.calibration_sha256 = response.at("calibration_sha256").get<std::string>();
  result.run_uuid = response.at("run_uuid").get<std::string>();
  if (!response.at("calibration_authority").is_number_unsigned() ||
      (response.at("calibration_authority") != 1 && response.at("calibration_authority") != 2))
    throw std::runtime_error("invalid native calibration authority");
  const auto authority = response.at("calibration_authority").get<unsigned>();
  result.unchanged = response.at("unchanged").get<bool>();
  std::string uuid_hex;
  for (std::size_t i = 0; i < result.run_uuid.size(); ++i) {
    if (result.run_uuid.size() == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
      if (result.run_uuid[i] != '-')
        throw std::runtime_error("invalid native calibration run UUID");
    } else {
      uuid_hex += result.run_uuid[i];
    }
  }
  if (result.calibration_sha256.size() != 64 || !hex(result.calibration_sha256) ||
      (authority != 1 && authority != 2) ||
      (code != "activated" && code != "unchanged") ||
      result.unchanged != (code == "unchanged") ||
      (result.run_uuid.size() != 32 && result.run_uuid.size() != 36) ||
      uuid_hex.size() != 32 || !hex(uuid_hex))
    throw std::runtime_error("invalid native calibration verification identity");
  result.calibration_authority = authority;
  return result;
}
}  // namespace

CalibrationActivation activate_local_calibration(const std::filesystem::path& bundle,
    const std::filesystem::path& path, std::chrono::milliseconds timeout) {
  if (timeout < std::chrono::milliseconds{1} || timeout > std::chrono::minutes{10})
    throw std::invalid_argument("calibration timeout must be within 1 ms and 10 minutes");
  auto payload = archive(bundle);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto name = path.string();
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (name.empty() || name.size() >= sizeof(address.sun_path))
    throw std::invalid_argument("invalid local calibration control socket path");
  std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
  Fd socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (socket.value < 0) throw std::runtime_error("cannot create native calibration control socket");
  const auto connect_deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds{1});
  while (::connect(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    if (errno == EISCONN) break;
    if (errno == EINPROGRESS) {
      wait(socket.value, POLLOUT, connect_deadline);
      int error{};
      socklen_t size = sizeof(error);
      if (::getsockopt(socket.value, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0) break;
      throw std::runtime_error("native calibration control connection failed");
    }
    if ((errno != EAGAIN && errno != EINTR) || std::chrono::steady_clock::now() >= connect_deadline)
      throw std::runtime_error(
          "cannot connect native calibration control; check nuvo-depth-calibration-control.socket, "
          "the read-only runtime mount and numeric nuvo-video group. Activate remote bundles on "
          "the host using sudo nuvoctl calibration import BUNDLE, then omit calibration_file");
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  ucred peer{};
  socklen_t peer_size = sizeof(peer);
  if (::getsockopt(socket.value, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
      peer_size != sizeof(peer) || peer.uid != 0)
    throw std::runtime_error("native calibration broker must be owned by root");
  std::array<std::byte, 12> request{};
  std::memcpy(request.data(), "NVC1", 4);
  put(request.data() + 4, payload.size(), 8);
  transfer(socket.value, request, true, deadline);
  transfer(socket.value, payload, true, deadline);
  std::array<std::byte, 8> header{};
  transfer(socket.value, header, false, deadline);
  if (std::memcmp(header.data(), "NCA1", 4) != 0)
    throw std::runtime_error("unsupported native calibration control reply");
  const auto size = get(header.data() + 4);
  if (size == 0 || size > 65536)
    throw std::runtime_error("native calibration reply exceeds bounds");
  std::vector<std::byte> reply(size);
  transfer(socket.value, reply, false, deadline);
  return decode(reply);
}
#else
CalibrationActivation activate_local_calibration(const std::filesystem::path&,
    const std::filesystem::path&, std::chrono::milliseconds) {
  throw std::runtime_error("native calibration activation requires the same-host Linux control socket");
}
#endif
}  // namespace nuvo::client
