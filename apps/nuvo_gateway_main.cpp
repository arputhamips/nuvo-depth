#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <time.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <ifaddrs.h>
#include <arpa/inet.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "depthd/local_frame_server.hpp"
#include "depthd/gateway.hpp"
#include "depthd/configuration_revision.hpp"
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
#include "depthd/image_encoder.hpp"
#endif

namespace {

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

struct Arguments {
  std::string address{"0.0.0.0"};
  std::uint16_t port{7443U};
  std::filesystem::path certificate;
  std::filesystem::path private_key;
  std::filesystem::path token_file;
  std::filesystem::path web_root{"/usr/share/nuvo-depth/web"};
  std::filesystem::path status_file{"/run/nuvo-depth/status.json"};
  std::filesystem::path snapshot_file{
      "/run/nuvo-depth/snapshots/latest.nuv"};
  bool diagnostic_snapshot_fallback{};
  std::filesystem::path data_socket{depthd::ipc_v2::kDataSocketPath};
  std::filesystem::path encoded_file{
      "/run/nuvo-depth/snapshots/latest.nve"};
  std::filesystem::path encoding_config_file{
      "/etc/nuvo-depth/encoding.json"};
  std::filesystem::path stereo_control_file{
      "/etc/nuvo-depth/stereo.json"};
  std::filesystem::path exposure_control_file{
      "/etc/nuvo-depth/exposure.json"};
  std::filesystem::path transport_file{"/etc/nuvo-depth/transport.json"};
  std::filesystem::path calibration_state_root{"/var/lib/nuvo-depth"};
  std::filesystem::path system_agent_socket{
      "/run/nuvo-system-agent.sock"};
  std::size_t maximum_sessions{16U};
  std::filesystem::path qualification_file{"/etc/nuvo-depth/gateway-qualification.json"};
  std::string engineering_interface;
  std::uint64_t engineering_byte_budget{64U * 1024U * 1024U};
  std::uint64_t wifi_byte_budget{8U * 1024U * 1024U};
};

std::atomic_bool stopping{};
extern "C" void stop(int) { stopping.store(true); }

[[nodiscard]] std::uint32_t number(const std::string_view value,
                                   const std::string_view option) {
  std::size_t used{};
  const auto parsed = std::stoul(std::string(value), &used, 10);
  if (used != value.size() || parsed == 0U || parsed > 65'535U) {
    throw std::invalid_argument(std::string(option) + " is out of range");
  }
  return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] Arguments arguments(const int argc, char** argv) {
  Arguments result;
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(argv[index]) +
                                  " requires a value");
    }
    const std::string_view option(argv[index]);
    const std::string_view value(argv[index + 1]);
    if (option == "--address") result.address = value;
    else if (option == "--port") result.port = static_cast<std::uint16_t>(number(value, option));
    else if (option == "--certificate") result.certificate = value;
    else if (option == "--private-key") result.private_key = value;
    else if (option == "--token-file") result.token_file = value;
    else if (option == "--web-root") result.web_root = value;
    else if (option == "--status-file") result.status_file = value;
    else if (option == "--snapshot-file") {
      result.snapshot_file = value;
      result.diagnostic_snapshot_fallback = true;
    }
    else if (option == "--data-socket") result.data_socket = value;
    else if (option == "--encoded-file") result.encoded_file = value;
    else if (option == "--encoding-config-file") result.encoding_config_file = value;
    else if (option == "--stereo-control-file") result.stereo_control_file = value;
    else if (option == "--exposure-control-file") result.exposure_control_file = value;
    else if (option == "--transport-file") result.transport_file = value;
    else if (option == "--calibration-state-root") result.calibration_state_root = value;
    else if (option == "--system-agent-socket") result.system_agent_socket = value;
    else if (option == "--maximum-sessions") result.maximum_sessions = number(value, option);
    else if (option == "--qualification-file") result.qualification_file = value;
    else if (option == "--engineering-interface") result.engineering_interface = value;
    else if (option == "--engineering-byte-budget" || option == "--wifi-byte-budget") {
      std::size_t used{};
      const auto budget = std::stoull(std::string(value), &used);
      if (used != value.size() || budget == 0U ||
          budget > 1024ULL * 1024ULL * 1024ULL) {
        throw std::invalid_argument("engineering byte budget is out of range");
      }
      if (option == "--wifi-byte-budget") result.wifi_byte_budget = budget;
      else result.engineering_byte_budget = budget;
    }
    else throw std::invalid_argument("unknown option: " + std::string(option));
  }
  if (result.certificate.empty() || result.private_key.empty() ||
      result.token_file.empty() || result.maximum_sessions > 128U) {
    throw std::invalid_argument(
        "--certificate, --private-key, and --token-file are required");
  }
  if (result.engineering_interface.size() > 15U ||
      result.engineering_interface.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos) {
    throw std::invalid_argument("invalid engineering interface name");
  }
  return result;
}

[[nodiscard]] std::vector<unsigned char> read_file(
    const std::filesystem::path& path, const std::size_t maximum) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open " + path.string());
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uint64_t>(end) > maximum) {
    throw std::length_error("file exceeds configured response bound");
  }
  std::vector<unsigned char> result(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(result.data()), end);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  return result;
}

[[nodiscard]] std::string token(const std::filesystem::path& path) {
  const auto bytes = read_file(path, 4096U);
  std::string result(bytes.begin(), bytes.end());
  while (!result.empty() && (result.back() == '\r' || result.back() == '\n' ||
                             result.back() == ' ' || result.back() == '\t')) {
    result.pop_back();
  }
  if (result.size() < 32U || result.size() > 256U) {
    throw std::invalid_argument("claim token must contain 32 through 256 bytes");
  }
  return result;
}

[[nodiscard]] bool equal_secret(const std::string_view left,
                                const std::string_view right) {
  return left.size() == right.size() &&
         CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

[[nodiscard]] std::string random_session() {
  unsigned char bytes[32]{};
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
    throw std::runtime_error("secure random session generation failed");
  }
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(64U);
  for (const auto value : bytes) {
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
  }
  return result;
}

class Sessions {
 public:
  struct Created { std::string session; std::string csrf; };
  explicit Sessions(const std::size_t maximum) : maximum_(maximum) {}
  [[nodiscard]] std::optional<Created> create() {
    std::lock_guard lock(mutex_);
    prune_locked();
    if (values_.size() >= maximum_) return std::nullopt;
    Created value{random_session(), random_session()};
    values_.emplace(value.session,
                    Entry{value.csrf, std::chrono::steady_clock::now() +
                                          std::chrono::hours(12)});
    return value;
  }
  [[nodiscard]] bool contains(const std::string_view value) const {
    std::lock_guard lock(mutex_);
    prune_locked();
    return values_.contains(std::string(value));
  }
  void remove(const std::string& session) {
    std::lock_guard lock(mutex_);
    values_.erase(session);
  }
  [[nodiscard]] std::optional<std::string> csrf_for(
      const std::string_view session) const {
    std::lock_guard lock(mutex_);
    prune_locked();
    const auto found = values_.find(std::string(session));
    if (found == values_.end()) return std::nullopt;
    return found->second.csrf;
  }
  [[nodiscard]] bool valid_csrf(const std::string_view session,
                                const std::string_view csrf) const {
    std::lock_guard lock(mutex_);
    prune_locked();
    const auto found = values_.find(std::string(session));
    return found != values_.end() && equal_secret(found->second.csrf, csrf);
  }
 private:
  struct Entry {
    std::string csrf;
    std::chrono::steady_clock::time_point expires;
  };
  void prune_locked() const {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(values_, [now](const auto& item) {
      return item.second.expires <= now;
    });
  }
  std::size_t maximum_;
  mutable std::mutex mutex_;
  mutable std::unordered_map<std::string, Entry> values_;
};

[[nodiscard]] std::string_view cookie_value(const std::string_view cookies,
                                            const std::string_view name) {
  auto remaining = cookies;
  while (!remaining.empty()) {
    const auto separator = remaining.find(';');
    auto item = remaining.substr(0, separator);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
      item.remove_prefix(1U);
    }
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
      item.remove_suffix(1U);
    }
    const auto equals = item.find('=');
    if (equals != std::string_view::npos && item.substr(0, equals) == name) {
      return item.substr(equals + 1U);
    }
    if (separator == std::string_view::npos) break;
    remaining.remove_prefix(separator + 1U);
  }
  return {};
}

template <class Body>
void security_headers(http::response<Body>& response) {
  response.set(http::field::server, "nuvo-gateway/0.1");
  response.set("Content-Security-Policy",
               "default-src 'self'; script-src 'self'; style-src 'self'; "
               "img-src 'self' data:; connect-src 'self'; object-src 'none'; "
               "base-uri 'none'; frame-ancestors 'none'");
  response.set("Referrer-Policy", "no-referrer");
  response.set("X-Content-Type-Options", "nosniff");
  response.set("X-Frame-Options", "DENY");
  response.set("Cache-Control", "no-store");
}

using Response = http::response<http::vector_body<unsigned char>>;

[[nodiscard]] Response response(http::status status,
                                const std::string_view content_type,
                                std::vector<unsigned char> body,
                                const unsigned version) {
  Response result{status, version};
  security_headers(result);
  result.set(http::field::content_type, content_type);
  result.body() = std::move(body);
  result.content_length(result.body().size());
  result.keep_alive(false);
  return result;
}

[[nodiscard]] Response text_response(const http::status status,
                                     const std::string_view type,
                                     const std::string_view body,
                                     const unsigned version) {
  return response(status, type,
                  std::vector<unsigned char>(body.begin(), body.end()), version);
}

[[nodiscard]] bool authenticated(
    const http::request<http::string_body>& request, const std::string& claim,
    const Sessions& sessions) {
  const auto authorization = request[http::field::authorization];
  constexpr std::string_view bearer = "Bearer ";
  if (authorization.size() > bearer.size() &&
      std::string_view(authorization).starts_with(bearer) &&
      equal_secret(std::string_view(authorization).substr(bearer.size()), claim)) {
    return true;
  }
  return sessions.contains(cookie_value(request[http::field::cookie],
                                        "nuvo_session"));
}

[[nodiscard]] bool valid_origin(
    const http::request<http::string_body>& request) {
  const auto origin = request[http::field::origin];
  if (origin.empty()) return true;
  const auto host = request[http::field::host];
  return std::string_view(origin) == "https://" + std::string(host);
}

[[nodiscard]] std::string transport_preference(const Arguments& options) {
  try {
    std::error_code error;
    if (!std::filesystem::exists(options.transport_file, error)) return "auto";
    const auto bytes = read_file(options.transport_file, 4096U);
    std::istringstream input{std::string(bytes.begin(), bytes.end())};
    boost::property_tree::ptree document;
    boost::property_tree::read_json(input, document);
    const auto preference = document.get<std::string>("preference");
    if (preference == "ethernet" || preference == "usb" ||
        preference == "both" || preference == "off" ||
        preference == "auto" || preference == "wifi") {
      return preference;
    }
  } catch (const std::exception&) {
  }
  return "off";
}

[[nodiscard]] std::string request_preference(const std::string_view body) {
  const auto key = body.find("\"preference\"");
  if (key == std::string_view::npos) {
    throw std::invalid_argument("request requires a preference field");
  }
  const auto colon = body.find(':', key + 12U);
  if (colon == std::string_view::npos) {
    throw std::invalid_argument("preference field is malformed");
  }
  const auto quote = body.find('"', colon);
  if (quote == std::string_view::npos) {
    throw std::invalid_argument("preference field is malformed");
  }
  const auto end = body.find('"', quote + 1U);
  if (end == std::string_view::npos) {
    throw std::invalid_argument("preference field is malformed");
  }
  const auto value = std::string(body.substr(quote + 1U, end - quote - 1U));
  if (value != "ethernet" && value != "usb" && value != "both" &&
      value != "off" && value != "auto" && value != "wifi") {
    throw std::invalid_argument("unknown transport preference");
  }
  return value;
}

[[nodiscard]] bool valid_csrf(
    const http::request<http::string_body>& request,
    const Sessions& sessions) {
  const auto session = cookie_value(request[http::field::cookie],
                                    "nuvo_session");
  return sessions.valid_csrf(session, request["X-Nuvo-CSRF"]);
}

[[nodiscard]] std::vector<unsigned char> system_agent(
    const Arguments& options, const std::string_view request,
    const int timeout_seconds = 310) {
  auto connection = depthd::ipc_v2::connect_seqpacket(
      options.system_agent_socket);
  const auto bytes = std::span(
      reinterpret_cast<const std::byte*>(request.data()), request.size());
  connection.send_packet(bytes);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  while (true) {
    if (stopping.load()) throw std::runtime_error("gateway is stopping");
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error("system agent response timed out");
    }
    pollfd descriptor{connection.native_handle(), POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, 100);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) throw std::runtime_error("system agent response poll failed");
    if (ready > 0) break;
  }
  const auto response = connection.receive_packet(64U * 1024U, 0U,
                                                  std::chrono::milliseconds(0));
  return {reinterpret_cast<const unsigned char*>(response.payload.data()),
          reinterpret_cast<const unsigned char*>(response.payload.data() +
                                                  response.payload.size())};
}

[[nodiscard]] bool agent_succeeded(
    const std::vector<unsigned char>& result) {
  const std::string_view document(
      reinterpret_cast<const char*>(result.data()), result.size());
  return document.find("\"status\":\"ok\"") != std::string_view::npos;
}

[[nodiscard]] std::string calibration_agent_request(
    const std::string_view operation, const std::string_view body) {
  if (body.size() > 8U * 1024U ||
      body.find("\"operation\"") != std::string_view::npos) {
    throw std::invalid_argument("invalid calibration request");
  }
  const auto first = body.find_first_not_of(" \t\r\n");
  const auto last = body.find_last_not_of(" \t\r\n");
  if (first == std::string_view::npos || body[first] != '{' ||
      body[last] != '}') {
    throw std::invalid_argument("calibration request must be a JSON object");
  }
  const auto interior = body.substr(first + 1U, last - first - 1U);
  return "{\"operation\":\"" + std::string(operation) + "\"" +
         (interior.find_first_not_of(" \t\r\n") == std::string_view::npos
              ? "}"
              : "," + std::string(interior) + "}");
}

[[nodiscard]] std::optional<std::string> calibration_session_query(
    const std::string_view target, const std::string_view path) {
  const auto prefix = std::string(path) + "?session=";
  if (!target.starts_with(prefix)) return std::nullopt;
  auto value = target.substr(prefix.size());
  if (const auto separator = value.find('&'); separator != std::string_view::npos) {
    const auto suffix = value.substr(separator);
    if (!suffix.starts_with("&v=") || suffix.size() > 32U ||
        !std::all_of(suffix.begin() + 3, suffix.end(), [](const char byte) {
          return byte >= '0' && byte <= '9';
        })) {
      return std::nullopt;
    }
    value = value.substr(0U, separator);
  }
  if (value.size() != 32U || !std::all_of(value.begin(), value.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
      })) {
    return std::nullopt;
  }
  return std::string(value);
}

#if defined(DEPTHD_WITH_IMAGE_ENCODER)
[[nodiscard]] depthd::encoding::Configuration encoding_configuration(
    const Arguments& options) {
  depthd::encoding::Configuration fallback;
  fallback.mode = depthd::encoding::StreamMode::kRawOnly;
  return depthd::encoding::load_configuration(options.encoding_config_file,
                                               fallback);
}
#endif

using Json = boost::property_tree::ptree;

[[nodiscard]] Json parse_json(const std::string_view document) {
  std::istringstream input{std::string(document)};
  Json result;
  boost::property_tree::read_json(input, result);
  return result;
}

[[nodiscard]] Json setup_agent(const Arguments& options, const std::string& operation,
                               const std::string& field = {}, const std::string& secret = {}) {
  Json request;
  request.put("operation", operation);
  if (!field.empty()) request.put(field, secret);
  std::ostringstream encoded;
  boost::property_tree::write_json(encoded, request, false);
  const auto reply = system_agent(options, encoded.str(), 10);
  const auto result = parse_json(std::string(reply.begin(), reply.end()));
  if (result.get<std::string>("status", "") != "ok") {
    throw std::runtime_error("setup operation unavailable");
  }
  return result.get_child("result");
}

[[nodiscard]] bool needs_owner_password(const Arguments& options) {
  try { return setup_agent(options, "setup.status").get<bool>("needs_owner_password", false); }
  catch (const std::exception&) { return false; } // Legacy installations have no setup state.
}

[[nodiscard]] depthd::ipc_v2::ProductMask product_mask(const std::string_view name) {
  using Kind = depthd::ipc_v2::ProductKind;
  for (const auto& [text, kind] : std::initializer_list<std::pair<std::string_view, Kind>>{
      {"left_rectified", Kind::kLeftRectified}, {"right_rectified", Kind::kRightRectified},
      {"disparity", Kind::kDisparity}, {"depth", Kind::kDepth},
      {"confidence", Kind::kConfidence}, {"validity", Kind::kValidity},
      {"left_mono", Kind::kLeftMono}, {"right_mono", Kind::kRightMono},
      {"left_rgb", Kind::kLeftRgb}, {"right_rgb", Kind::kRightRgb},
      {"left_rectified_rgb", Kind::kLeftRectifiedRgb}, {"right_rectified_rgb", Kind::kRightRectifiedRgb},
      {"rectified_stereo", Kind::kRectifiedStereo}, {"rectified_stereo_rgb", Kind::kRectifiedStereoRgb}}) {
    if (name == text) return depthd::ipc_v2::product_bit(kind);
  }
  throw std::invalid_argument("unknown stream product");
}

struct Subscription {
  depthd::ipc_v2::ProductMask products{depthd::ipc_v2::kAllProducts};
  double cadence_hz{10.0};
  bool exact{};
  bool automatic{};
  std::string delivery{"auto"};
};

[[nodiscard]] Subscription subscription(const std::string_view target, const bool encoded = false) {
  Subscription result;
  const auto separator = target.find('?');
  if (separator == std::string_view::npos) return result;
  auto query = target.substr(separator + 1U);
  bool cadence{}, products{}, delivery{};
  while (!query.empty()) {
    const auto next = query.find('&');
    const auto item = query.substr(0U, next);
    const auto equals = item.find('=');
    if (equals == std::string_view::npos) throw std::invalid_argument("malformed subscription");
    const auto key = item.substr(0U, equals);
    auto value = item.substr(equals + 1U);
    if (key == "cadence_hz" && !cadence) {
      std::size_t used{};
      result.cadence_hz = std::stod(std::string(value), &used);
      if (used != value.size() || !std::isfinite(result.cadence_hz) ||
          result.cadence_hz <= 0.0 || result.cadence_hz > 60.0) {
        throw std::invalid_argument("cadence_hz must be greater than zero and at most 60");
      }
      cadence = true;
    } else if (key == "products" && !products && !value.empty()) {
      result.products = 0U;
      while (!value.empty()) {
        const auto comma = value.find(',');
        const auto bit = product_mask(value.substr(0U, comma));
        if ((result.products & bit) != 0U) throw std::invalid_argument("duplicate product");
        result.products |= bit;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1U);
        if (value.empty()) throw std::invalid_argument("empty product");
      }
      products = true;
    } else if (key == "delivery" && !delivery &&
               (value == "auto" || value == "best_effort" || value == "exact")) {
      result.delivery = value;
      result.automatic = value == "auto";
      delivery = true;
    } else {
      throw std::invalid_argument("unknown or duplicate subscription parameter");
    }
    if (next == std::string_view::npos) break;
    query.remove_prefix(next + 1U);
    if (query.empty()) throw std::invalid_argument("empty subscription parameter");
  }
  if (encoded) {
    if (products || result.delivery == "exact") throw std::invalid_argument("encoded preview is best effort and has no raw product mask");
  } else if ((cadence || products) && (!cadence || !products)) {
    throw std::invalid_argument("raw subscription requires products and cadence_hz");
  }
  result.exact = !encoded && result.delivery != "best_effort" && (cadence || result.delivery == "exact");
  return result;
}

[[nodiscard]] std::string receiving_interface(const tcp::socket& socket) {
  const auto local = socket.local_endpoint().address();
  ifaddrs* addresses{};
  if (::getifaddrs(&addresses) != 0) throw std::runtime_error("cannot inspect receiving interface");
  std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> owner(addresses, &::freeifaddrs);
  for (auto* item = addresses; item; item = item->ifa_next) {
    if (!item->ifa_addr) continue;
    char address[INET6_ADDRSTRLEN]{};
    const void* bytes{};
    if (item->ifa_addr->sa_family == AF_INET) {
      bytes = &reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr;
    } else if (item->ifa_addr->sa_family == AF_INET6) {
      bytes = &reinterpret_cast<sockaddr_in6*>(item->ifa_addr)->sin6_addr;
    } else continue;
    if (::inet_ntop(item->ifa_addr->sa_family, bytes, address, sizeof(address)) &&
        net::ip::make_address(address) == local) return item->ifa_name;
  }
  throw std::runtime_error("accepted socket has no receiving interface");
}

using FrameBytes = depthd::gateway::SerializedBytes;

[[nodiscard]] Json frame_metadata(const FrameBytes& bytes) {
  if (bytes.size() < depthd::wire::kHeaderBytes) throw std::invalid_argument("short frame");
  const auto header = depthd::wire::decode_header(bytes.prefix().first(depthd::wire::kHeaderBytes));
  if (header.kind != depthd::wire::MessageKind::kFrameSet ||
      bytes.size() != depthd::wire::kHeaderBytes + header.metadata_bytes + header.payload_bytes) {
    throw std::invalid_argument("invalid frame-set length");
  }
  return parse_json(std::string_view(reinterpret_cast<const char*>(bytes.data() +
      depthd::wire::kHeaderBytes), header.metadata_bytes));
}

// Validate the NVE1 envelope before reading bounded metadata. Arrival over a
// new IPC connection does not make a cached encoded capture fresh.
[[nodiscard]] std::uint64_t encoded_capture_time(const FrameBytes& bytes) {
  constexpr std::size_t header_bytes = 32U;
  const auto prefix = bytes.prefix();
  if (prefix.size() < header_bytes || bytes.size() > 4U * 1024U * 1024U ||
      std::memcmp(prefix.data(), "NVE1", 4U) != 0)
    throw std::invalid_argument("invalid encoded frame envelope");
  const auto read = [&](std::size_t offset, std::size_t count) {
    std::uint64_t value{};
    for (std::size_t index = offset; index < offset + count; ++index)
      value = (value << 8U) | std::to_integer<std::uint8_t>(prefix[index]);
    return value;
  };
  const auto flags = read(6U, 2U), metadata_bytes = read(8U, 4U), payload_bytes = read(12U, 8U);
  if (read(4U, 2U) != 1U || (flags & ~1U) != 0U || read(28U, 4U) != 0U ||
      read(20U, 8U) == 0U || metadata_bytes == 0U || metadata_bytes > 65536U ||
      payload_bytes == 0U || payload_bytes > 32U * 1024U * 1024U ||
      header_bytes + metadata_bytes + payload_bytes != bytes.size() ||
      header_bytes + metadata_bytes > prefix.size())
    throw std::invalid_argument("invalid encoded frame bounds or header");
  const std::string_view document(reinterpret_cast<const char*>(prefix.data() + header_bytes), metadata_bytes);
  const auto metadata = parse_json(document);
  std::unordered_set<std::string> fields, quoted_fields;
  // property_tree stores JSON scalar types as strings. Preserve the literal
  // type here so e.g. schema_version="1" cannot pass and then fail SDK decode.
  const auto skip_space = [&](std::size_t position) { return document.find_first_not_of(" \t\r\n", position); };
  const auto skip_string = [&](std::size_t position) {
    if (position >= document.size() || document[position++] != '"')
      throw std::invalid_argument("invalid encoded metadata object");
    while (position < document.size()) {
      const auto character = document[position++];
      if (character == '\\') ++position;
      else if (character == '"') return position;
    }
    throw std::invalid_argument("invalid encoded metadata string");
  };
  auto position = skip_space(0U);
  if (position >= document.size() || document[position++] != '{')
    throw std::invalid_argument("encoded metadata is not an object");
  for (const auto& [key, value] : metadata) {
    if (key.empty() || !value.empty() || !fields.insert(key).second)
      throw std::invalid_argument("encoded metadata must contain unique scalar fields");
    position = skip_space(skip_string(skip_space(position)));
    if (position >= document.size() || document[position++] != ':')
      throw std::invalid_argument("invalid encoded metadata member");
    position = skip_space(position);
    if (position >= document.size() || document[position] == '{' || document[position] == '[')
      throw std::invalid_argument("encoded metadata member is not scalar");
    if (document[position] == '"') { quoted_fields.insert(key); position = skip_string(position); }
    else position = document.find_first_of(",}", position);
    position = skip_space(position);
    if (position < document.size() && document[position] == ',') ++position;
  }
  const auto positive = [&](const char* field) {
    const auto value = metadata.get<std::string>(field);
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (quoted_fields.contains(field) || result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0U)
      throw std::invalid_argument("invalid encoded metadata integer");
    return parsed;
  };
  const auto valid_uuid = [&](const char* field) {
    const auto value = metadata.get<std::string>(field);
    if (!quoted_fields.contains(field) || (value.size() != 32U && value.size() != 36U)) return false;
    bool nonzero{};
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (value.size() == 36U && (index == 8U || index == 13U || index == 18U || index == 23U)) {
        if (value[index] != '-') return false;
      } else {
        if (std::string_view("0123456789abcdefABCDEF").find(value[index]) == std::string_view::npos) return false;
        nonzero |= value[index] != '0';
      }
    }
    return nonzero;
  };
  const auto keyframe = metadata.get<std::string>("keyframe");
  const auto source = metadata.get<std::string>("source");
  constexpr std::array sources{"left_rectified", "right_rectified", "left_color", "right_color",
      "left_mono", "right_mono", "left_rgb", "right_rgb", "left_rectified_rgb", "right_rectified_rgb",
      "rectified_stereo", "rectified_stereo_rgb", "disparity", "depth"};
  const auto codec = metadata.get<std::string>("codec"), mime = metadata.get<std::string>("mime_type");
  for (const auto* field : {"capture_epoch", "capture_sequence", "configuration_epoch"}) (void)positive(field);
  if (positive("schema_version") != 1U || positive("capture_clock") != 1U ||
      !valid_uuid("device_uuid") || !valid_uuid("run_uuid") ||
      positive("width") > 32768U || positive("height") > 32768U ||
      !quoted_fields.contains("source") || !quoted_fields.contains("codec") || !quoted_fields.contains("mime_type") ||
      std::find(sources.begin(), sources.end(), source) == sources.end() ||
      !((codec == "jpeg" && mime == "image/jpeg") || (codec == "h264" && mime == "video/h264")) ||
      quoted_fields.contains("keyframe") || (keyframe != "true" && keyframe != "false") ||
      (keyframe == "true") != ((flags & 1U) != 0U))
    throw std::invalid_argument("invalid encoded frame metadata");
  return positive("capture_time_ns");
}

[[nodiscard]] bool fresh_capture(const std::uint64_t captured) {
  timespec current{};
  if (::clock_gettime(CLOCK_BOOTTIME, &current) != 0)
    throw std::runtime_error("capture freshness clock unavailable");
  const auto now = static_cast<std::uint64_t>(current.tv_sec) * 1'000'000'000U +
      static_cast<std::uint64_t>(current.tv_nsec);
  return captured != 0U && captured <= now && now - captured <= 1'000'000'000U;
}

// A subset is serialized once per source publication and mask. Identity and
// non-product provenance remains byte-for-byte identical to the source. The
// selected mask changes with its descriptors; the available union is retained.
[[nodiscard]] std::shared_ptr<const FrameBytes> select_products(
    const std::shared_ptr<const FrameBytes>& bytes,
    const depthd::ipc_v2::ProductMask mask) {
  auto header = depthd::wire::decode_header(bytes->prefix().first(depthd::wire::kHeaderBytes));
  const auto metadata = frame_metadata(*bytes);
  const auto payload = bytes->prefix().subspan(depthd::wire::kHeaderBytes + header.metadata_bytes);
  std::string document(reinterpret_cast<const char*>(bytes->data() + depthd::wire::kHeaderBytes),
                       header.metadata_bytes);
  const auto field = document.find("\"products\"");
  const auto first = document.find('[', field);
  if (field == std::string::npos || first == std::string::npos) throw std::invalid_argument("missing product metadata");
  auto last = first;
  unsigned depth{};
  bool quoted{}, escaped{};
  for (; last < document.size(); ++last) {
    const auto character = document[last];
    if (quoted) {
      if (escaped) escaped = false;
      else if (character == '\\') escaped = true;
      else if (character == '"') quoted = false;
    } else if (character == '"') quoted = true;
    else if (character == '[') ++depth;
    else if (character == ']' && --depth == 0U) break;
  }
  if (last == document.size()) throw std::invalid_argument("unterminated product metadata");
  std::ostringstream selected;
  selected << '[';
  std::vector<std::span<const std::byte>> slices;
  std::size_t total{};
  depthd::ipc_v2::ProductMask found{};
  for (const auto& item : metadata.get_child("products")) {
    const auto& product = item.second;
    const auto name = product.get<std::string>("kind");
    const auto bit = product_mask(name);
    const auto offset = product.get<std::size_t>("offset");
    const auto length = product.get<std::size_t>("length");
    if (offset > payload.size() || length > payload.size() - offset) throw std::invalid_argument("product outside payload");
    if ((bit & mask) == 0U) continue;
    if ((found & bit) != 0U) throw std::invalid_argument("duplicate frame product");
    const auto format = product.get<std::string>("format");
    if (format != "gray8" && format != "bgr8" && format != "rgb8" &&
        format != "float32_le" && format != "uint8") throw std::invalid_argument("unsupported product format");
    if (found != 0U) selected << ',';
    selected << "{\"kind\":\"" << name << "\",\"format\":\"" << format
             << "\",\"offset\":" << total << ",\"length\":" << length
             << ",\"width\":" << product.get<std::uint32_t>("width")
             << ",\"height\":" << product.get<std::uint32_t>("height") << '}';
    found |= bit;
    slices.push_back(payload.subspan(offset, length));
    total += length;
  }
  if (found != mask) throw std::invalid_argument("requested product unavailable");
  selected << ']';
  document.replace(first, last - first + 1U, selected.str());
  if (metadata.get_optional<unsigned>("selected_products")) {
    const auto key = document.find("\"selected_products\"");
    auto start = document.find(':', key) + 1U;
    start = document.find_first_not_of(" \t\r\n", start);
    const auto end = document.find_first_not_of("0123456789", start);
    if (key == std::string::npos || start == std::string::npos || end == start) throw std::invalid_argument("invalid selected product mask metadata");
    document.replace(start, end - start, std::to_string(mask));
  }
  header.metadata_bytes = static_cast<std::uint32_t>(document.size());
  header.payload_bytes = total;
  const auto encoded = depthd::wire::encode_header(header);
  std::vector<std::byte> prefix;
  prefix.reserve(depthd::wire::kHeaderBytes + document.size());
  prefix.insert(prefix.end(), encoded.begin(), encoded.end());
  const auto* start = reinterpret_cast<const std::byte*>(document.data());
  prefix.insert(prefix.end(), start, start + document.size());
  return std::make_shared<const FrameBytes>(std::move(prefix), bytes, std::move(slices));
}

// One local reader and one encoded-file watcher feed immutable latest values.
// Clients wait for publications rather than each polling and reading files.
class FrameSource final {
 public:
  using Bytes = FrameBytes;
  struct Snapshot {
    std::uint64_t generation{};
    std::shared_ptr<const Bytes> bytes;
    depthd::ipc_v2::ProductMask products{};
    std::chrono::steady_clock::time_point published_at{};
    std::uint64_t capture_time_ns{};
  };
  class DemandLease final {
   public:
    DemandLease() = default;
    DemandLease(const DemandLease&) = delete;
    DemandLease& operator=(const DemandLease&) = delete;
    DemandLease(DemandLease&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), id_(other.id_) {}
    DemandLease& operator=(DemandLease&& other) noexcept {
      if (this != &other) { reset(); owner_ = std::exchange(other.owner_, nullptr); id_ = other.id_; }
      return *this;
    }
    ~DemandLease() { reset(); }
    void reset() noexcept { if (owner_) { owner_->release(id_); owner_ = nullptr; } }
   private:
    DemandLease(FrameSource& owner, std::uint64_t id) : owner_(&owner), id_(id) {}
    FrameSource* owner_{};
    std::uint64_t id_{};
    friend class FrameSource;
  };
  explicit FrameSource(const Arguments& options) : options_(options) {
    refresh_policy();
    raw_worker_ = std::thread([this] { read_raw(false); });
    try {
      matcher_worker_ = std::thread([this] { read_raw(true); });
      encoded_worker_ = std::thread([this] { read_encoded(); }); }
    catch (...) { stop(); throw; }
  }
  ~FrameSource() { stop(); }
  void stop() noexcept {
    stopped_.store(true); changed_.notify_all();
    if (raw_worker_.joinable()) raw_worker_.join();
    if (matcher_worker_.joinable()) matcher_worker_.join();
    if (encoded_worker_.joinable()) encoded_worker_.join();
  }
  [[nodiscard]] DemandLease acquire(bool encoded, depthd::ipc_v2::ProductMask products,
                                     double cadence) {
    if (!std::isfinite(cadence) || cadence <= 0 || cadence > 60 ||
        (!encoded && (products == 0 || (products & ~depthd::ipc_v2::kSupportedProducts) != 0))) {
      throw std::invalid_argument("invalid output demand");
    }
    // Configuration writes complete before their HTTP receipt, but the idle
    // worker may not have observed them yet. Refresh before deciding whether
    // this first consumer can start the requested source.
    refresh_policy();
    std::lock_guard lock(mutex_);
    if (stopped_.load()) throw std::runtime_error("local source is stopping");
    const auto id = ++demand_id_;
    demands_.emplace(id, Demand{encoded, products, cadence});
    changed_.notify_all();
    return DemandLease(*this, id);
  }
  [[nodiscard]] Snapshot wait_ready(bool encoded, depthd::ipc_v2::ProductMask products,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    std::unique_lock lock(mutex_);
    const auto ready = [&] {
      const auto value = latest_locked(encoded, products);
      return value.bytes && (encoded || (value.products & products) == products) &&
          std::chrono::steady_clock::now() - value.published_at <= std::chrono::milliseconds(500);
    };
    changed_.wait_for(lock, timeout, [&] { return stopped_.load() || stopping.load() || ready(); });
    return ready() ? latest_locked(encoded, products) : Snapshot{};
  }
  [[nodiscard]] bool enabled(bool encoded) const {
    std::lock_guard lock(mutex_);
    return transport_ != "off" && (encoded ? encoded_enabled_ : raw_enabled_);
  }
  [[nodiscard]] Snapshot latest(bool encoded, depthd::ipc_v2::ProductMask products = 0U) const {
    std::lock_guard lock(mutex_); return latest_locked(encoded, products);
  }
  [[nodiscard]] std::uint64_t subscribe(std::function<void()> callback) {
    std::lock_guard lock(mutex_); const auto id = ++subscriber_id_;
    subscribers_.emplace(id, std::move(callback)); return id;
  }
  void unsubscribe(std::uint64_t id) noexcept {
    std::lock_guard lock(mutex_); subscribers_.erase(id);
  }
  [[nodiscard]] Snapshot selected(const Snapshot& frame, depthd::ipc_v2::ProductMask mask) {
    if (!frame.bytes || mask == frame.products) return frame;
    if ((frame.products & mask) != mask) throw std::runtime_error("requested products are not published yet");
    std::lock_guard lock(subsets_mutex_);
    if (subset_generation_ > frame.generation) {
      return {frame.generation, select_products(frame.bytes, mask), mask, frame.published_at};
    }
    if (subset_generation_ != frame.generation) {
      subsets_.clear(); subset_bytes_ = frame.bytes->retained_size(); subset_generation_ = frame.generation;
    }
    if (const auto found = subsets_.find(mask); found != subsets_.end()) {
      return {frame.generation, found->second, mask, frame.published_at};
    }
    auto bytes = select_products(frame.bytes, mask);
    constexpr std::size_t maximum = 16U * 1024U * 1024U +
        4U * (depthd::wire::kHardMaximumMetadataBytes + depthd::wire::kHeaderBytes);
    for (auto item = subsets_.begin(); item != subsets_.end() &&
         (subsets_.size() >= 4 || subset_bytes_ + bytes->prefix().size() > maximum);) {
      if (item->second.use_count() == 1) { subset_bytes_ -= item->second->prefix().size(); item = subsets_.erase(item); }
      else ++item;
    }
    if (bytes->retained_size() > maximum || subsets_.size() >= 4 || subset_bytes_ + bytes->prefix().size() > maximum) {
      throw std::length_error("active product selection cache is full");
    }
    subset_bytes_ += bytes->prefix().size(); subsets_.emplace(mask, bytes);
    return {frame.generation, std::move(bytes), mask, frame.published_at};
  }
  [[nodiscard]] std::pair<std::size_t, std::size_t> consumers() const {
    std::lock_guard lock(mutex_); std::size_t raw{}, encoded{};
    for (const auto& entry : demands_) (entry.second.encoded ? encoded : raw)++;
    return {raw, encoded};
  }
 private:
  struct Demand { bool encoded; depthd::ipc_v2::ProductMask products; double cadence; };
  struct Aggregate { depthd::ipc_v2::ProductMask products{}; double cadence{}; };
  [[nodiscard]] Snapshot latest_locked(bool encoded, depthd::ipc_v2::ProductMask products) const {
    if (encoded) return encoded_;
    Snapshot result;
    for (const auto* candidate : {&raw_, &raw_matcher_})
      if (candidate->bytes && (candidate->products & products) == products &&
          (!result.bytes || candidate->capture_time_ns > result.capture_time_ns ||
           (candidate->capture_time_ns == result.capture_time_ns && candidate->generation > result.generation))) result = *candidate;
    return result;
  }
  [[nodiscard]] Aggregate aggregate(bool encoded, bool matcher = false) const {
    std::lock_guard lock(mutex_); Aggregate result;
    for (const auto& entry : demands_) {
      if (entry.second.encoded != encoded) continue;
      if (!encoded && ((entry.second.products & depthd::ipc_v2::kMatcherProducts) != 0U) != matcher) continue;
      result.products |= entry.second.products;
      result.cadence = std::max(result.cadence, entry.second.cadence);
    }
    return result;
  }
  void release(std::uint64_t id) noexcept {
    std::lock_guard lock(mutex_); demands_.erase(id);
    bool raw{}, encoded{};
    for (const auto& entry : demands_) (entry.second.encoded ? encoded : raw) = true;
    if (!raw) clear_locked(false);
    if (!encoded) clear_locked(true);
    changed_.notify_all();
  }
  static std::optional<std::filesystem::file_time_type> modified(const std::filesystem::path& path) noexcept {
    std::error_code error; const auto value = std::filesystem::last_write_time(path, error);
    return error ? std::nullopt : std::optional(value);
  }
  static Bytes byte_buffer(const std::vector<unsigned char>& bytes) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
  }
  void notify_locked() {
    changed_.notify_all();
    for (const auto& subscriber : subscribers_) subscriber.second();
  }
  void clear_locked(bool encoded) {
    if (!encoded) {
      std::lock_guard variants(subsets_mutex_);
      subsets_.clear(); subset_bytes_ = 0;
      raw_matcher_ = {};
    }
    auto& destination = encoded ? encoded_ : raw_;
    if (!destination.bytes) return;
    destination.bytes.reset(); destination.products = 0; ++destination.generation;
    notify_locked();
  }
  void clear(bool encoded) { std::lock_guard lock(mutex_); clear_locked(encoded); }
  void clear_raw_group(bool matcher) {
    std::lock_guard lock(mutex_);
    auto& value = matcher ? raw_matcher_ : raw_;
    if (value.bytes) {
      std::lock_guard variants(subsets_mutex_);
      if (subset_generation_ == value.generation) { subsets_.clear(); subset_bytes_ = 0U; }
      value = {}; notify_locked();
    }
  }
  void refresh_policy() noexcept {
    std::lock_guard policy(policy_mutex_);
    try {
      const auto transport_modified = modified(options_.transport_file);
      const auto encoding_modified = modified(options_.encoding_config_file);
      if (policy_initialized_ && transport_modified == transport_modified_ && encoding_modified == encoding_modified_) return;
      const auto preference = transport_preference(options_);
      bool raw = true, encoded = false;
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
      const auto configuration = encoding_configuration(options_);
      raw = depthd::encoding::raw_enabled(configuration); encoded = depthd::encoding::encoded_enabled(configuration);
#endif
      std::lock_guard lock(mutex_);
      if (policy_initialized_ && encoding_modified != encoding_modified_) clear_locked(true);
      transport_ = preference; raw_enabled_ = raw; encoded_enabled_ = encoded;
      policy_initialized_ = true; transport_modified_ = transport_modified; encoding_modified_ = encoding_modified;
      if (preference == "off" || !raw) clear_locked(false);
      if (preference == "off" || !encoded) clear_locked(true);
      // Disabled sources normally have no cached bytes, so clearing a cache
      // alone does not wake the worker when a source becomes enabled.
      notify_locked();
    } catch (...) {
      std::lock_guard lock(mutex_); raw_enabled_ = false; encoded_enabled_ = false;
      clear_locked(false); clear_locked(true); notify_locked();
    }
  }
  void publish(bool encoded, Bytes bytes, bool matcher = false) {
    depthd::ipc_v2::ProductMask products{};
    std::uint64_t capture_time{}; std::string scope;
    if (encoded) {
      try { capture_time = encoded_capture_time(bytes); }
      catch (const std::exception&) { return; }
      // read_encoded has already advanced its cursor. Skipping a stale or
      // malformed cached packet must preserve demand until a fresh one arrives.
      if (!fresh_capture(capture_time)) return;
    } else {
      const auto metadata = frame_metadata(bytes);
      capture_time = metadata.get<std::uint64_t>("capture_time_ns", 0U);
      scope = metadata.get<std::string>("run_uuid", "") + ":" +
          metadata.get<std::string>("capture_epoch", "") + ":" + metadata.get<std::string>("configuration_epoch", "");
      // A freshly retrieved cached memfd may still describe an old capture.
      // Native timestamps use CLOCK_BOOTTIME, so reject stale publications
      // before they can satisfy a new consumer's first-frame wait.
      if (metadata.get<std::string>("capture_clock", "") == "boottime") {
        // Keep the subscription and its advanced cursor while dropping old
        // captures. Reconnecting would fetch the same cached frame again and
        // withdraw demand before an idle producer can capture a fresh pair.
        if (!fresh_capture(capture_time)) return;
      }
      for (const auto& item : metadata.get_child("products")) {
        products |= product_mask(item.second.get<std::string>("kind"));
      }
    }
    auto value = std::make_shared<const Bytes>(std::move(bytes));
    std::lock_guard lock(mutex_);
    if (!encoded) {
      if (raw_scope_ != scope) {
        // Both readers share CLOCK_BOOTTIME, including across daemon runs.
        // An in-flight response from the old scope cannot roll the cache back.
        if (capture_time < raw_scope_time_) return;
        raw_ = {}; raw_matcher_ = {}; raw_scope_ = scope;
      }
      raw_scope_time_ = std::max(raw_scope_time_, capture_time);
    }
    auto& destination = encoded ? encoded_ : matcher ? raw_matcher_ : raw_;
    if (!encoded && destination.bytes && capture_time < destination.capture_time_ns) return;
    destination.generation = ++publication_generation_; destination.bytes = std::move(value); destination.products = products;
    destination.published_at = std::chrono::steady_clock::now();
    destination.capture_time_ns = capture_time;
    notify_locked();
  }
  void pause(std::chrono::milliseconds duration) {
    std::unique_lock lock(mutex_); changed_.wait_for(lock, duration);
  }
  void read_raw(bool matcher) noexcept {
    std::unique_ptr<depthd::ipc_v2::LocalFrameClient> client;
    depthd::ipc_v2::LocalFrameCursor cursor{}; Aggregate active;
    std::optional<std::filesystem::file_time_type> previous;
    while (!stopped_.load() && !stopping.load()) {
      refresh_policy(); const auto requested = aggregate(false, matcher);
      if (!enabled(false) || requested.products == 0) {
        client.reset(); cursor = {}; active = {}; previous.reset();
        clear_raw_group(matcher);
        pause(std::chrono::milliseconds(200)); continue;
      }
      try {
        if (!client) {
          client = std::make_unique<depthd::ipc_v2::LocalFrameClient>(options_.data_socket, requested.products, requested.cadence);
          active = requested; cursor = {};
        } else if (active.products != requested.products || active.cadence != requested.cadence) {
          client->set_demand(requested.products, requested.cadence); active = requested; cursor = {};
        }
        auto next = client->next_mapped(cursor, std::chrono::milliseconds(200), 16U * 1024U * 1024U);
        if (next) { cursor = next->cursor; publish(false, Bytes(std::move(next->owner), next->bytes), matcher); }
      } catch (...) {
        client.reset(); cursor = {};
        clear_raw_group(matcher);
        if (options_.diagnostic_snapshot_fallback) {
          try {
            const auto timestamp = modified(options_.snapshot_file);
            if (timestamp && timestamp != previous) {
              publish(false, byte_buffer(read_file(options_.snapshot_file, 16U * 1024U * 1024U)), matcher); previous = timestamp;
            }
          } catch (...) { }
        }
        pause(std::chrono::milliseconds(200));
      }
    }
  }
  void read_encoded() noexcept {
    std::unique_ptr<depthd::ipc_v2::LocalFrameClient> client;
    depthd::ipc_v2::LocalFrameCursor cursor{};
    double active{}; std::optional<std::filesystem::file_time_type> previous;
    std::chrono::steady_clock::time_point refreshed{};
    while (!stopped_.load() && !stopping.load()) {
      const auto requested = aggregate(true);
      if (!enabled(true) || requested.cadence == 0) {
        client.reset(); cursor = {}; active = 0; clear(true); previous.reset();
        pause(std::chrono::milliseconds(200)); continue;
      }
      try {
        if (!client) {
          client = std::make_unique<depthd::ipc_v2::LocalFrameClient>(options_.data_socket, 0, 0, requested.cadence);
          active = requested.cadence; cursor = {}; refreshed = std::chrono::steady_clock::now();
        } else if (active != requested.cadence || std::chrono::steady_clock::now() - refreshed >= std::chrono::seconds(2)) {
          client->set_demand(0, 0, requested.cadence); active = requested.cadence; refreshed = std::chrono::steady_clock::now();
        }
        auto next = client->next_encoded_mapped(cursor, std::chrono::milliseconds(200));
        if (next) {
          cursor = next->cursor;
          if (next->bytes.empty()) clear(true);
          else publish(true, Bytes(std::move(next->owner), next->bytes));
        }
      } catch (...) {
        client.reset(); cursor = {}; active = 0; clear(true);
        if (options_.diagnostic_snapshot_fallback) {
          try {
            const auto timestamp = modified(options_.encoded_file);
            if (timestamp && timestamp != previous) {
              publish(true, byte_buffer(read_file(options_.encoded_file, 4U * 1024U * 1024U))); previous = timestamp;
            }
          } catch (...) { }
        }
        pause(std::chrono::milliseconds(200));
      }
    }
  }
  const Arguments& options_;
  std::mutex policy_mutex_;
  mutable std::mutex mutex_; std::condition_variable changed_;
  Snapshot raw_, raw_matcher_, encoded_; std::string transport_, raw_scope_;
  std::uint64_t publication_generation_{};
  std::uint64_t raw_scope_time_{};
  bool raw_enabled_{true}, encoded_enabled_{}, policy_initialized_{};
  std::optional<std::filesystem::file_time_type> transport_modified_, encoding_modified_;
  std::atomic_bool stopped_{}; std::thread raw_worker_, matcher_worker_, encoded_worker_;
  std::uint64_t subscriber_id_{}, demand_id_{};
  std::unordered_map<std::uint64_t, std::function<void()>> subscribers_;
  std::unordered_map<std::uint64_t, Demand> demands_;
  std::mutex subsets_mutex_; std::uint64_t subset_generation_{}; std::size_t subset_bytes_{};
  std::unordered_map<depthd::ipc_v2::ProductMask, std::shared_ptr<const Bytes>> subsets_;
};

class StreamAdmission final {
 public:
  struct Contract {
    std::optional<depthd::gateway::AdmissionLease> lease;
    std::shared_ptr<std::size_t> memory;
    std::string identity;
    std::size_t maximum_wire_bytes{};
    bool qualified{};
    std::string preference;
    depthd::ipc_v2::ProductMask products{};
    depthd::gateway::TransportKind transport{};
    std::string label;
  };
  explicit StreamAdmission(const Arguments& options) : options_(options),
      qualification_configuration_(qualification(options)),
      traffic_configuration_(effective_configuration(options)), traffic_(traffic_configuration_) {}

  void preflight(const Subscription& requested, const std::string& interface,
                 const bool encoded) const {
    if (transport_preference(options_) == "off") throw std::runtime_error("all data transports disabled");
    const bool override = !options_.engineering_interface.empty() && interface == options_.engineering_interface;
    const bool wifi = is_wifi(interface);
    if (interface == "lo" && !override) throw std::runtime_error("unsupported data interface");
    if (requested.delivery == "exact" && (wifi || override)) throw std::runtime_error("this connection supports best effort only");
    if (encoded && requested.exact) throw std::invalid_argument("encoded streams do not expose raw product subscriptions");
  }

  [[nodiscard]] Contract admit(const Subscription& requested,
                              const std::string& interface,
                              const std::shared_ptr<const FrameSource::Bytes>& bytes,
                              const bool encoded) {
    preflight(requested, interface, encoded);
    const bool override = !options_.engineering_interface.empty() &&
                          interface == options_.engineering_interface;
    const bool wifi = is_wifi(interface);
    const bool wired = interface != "lo" && !wifi;
    const auto kind = wifi ? depthd::gateway::TransportKind::kWifi : is_usb(interface)
        ? depthd::gateway::TransportKind::kUsbEthernet
        : depthd::gateway::TransportKind::kEthernet;
    const bool known = wired && (interface == ethernet_interface_ || interface == usb_interface_);
    Contract result;
    result.preference = transport_preference(options_);
    result.products = requested.products;
    result.transport = kind;
    Json metadata;
    depthd::wire::Header header;
    if (!encoded) {
      metadata = frame_metadata(*bytes);
      header = depthd::wire::decode_header(bytes->prefix().first(depthd::wire::kHeaderBytes));
      result.identity = identity(metadata, requested.products);
      result.maximum_wire_bytes = depthd::wire::kHeaderBytes +
          depthd::wire::kHardMaximumMetadataBytes + static_cast<std::size_t>(header.payload_bytes);
    } else {
      result.maximum_wire_bytes = 4U * 1024U * 1024U;
    }
    const auto& qualified_link = kind == depthd::gateway::TransportKind::kUsbEthernet
        ? qualification_configuration_.usb_ethernet : qualification_configuration_.ethernet;
    const bool evidence_covers_request = !encoded && known && qualified_link.enabled && matches(metadata) &&
        requested.cadence_hz <= source_cadence_ && result.maximum_wire_bytes <= source_maximum_frame_ && qualification_current();
    result.qualified = requested.exact && !override && !wifi && (!requested.automatic || evidence_covers_request);
    result.label = override ? "engineering-best-effort" : result.qualified ? "qualified-exact" : "best-effort";
    if (result.qualified && (!known || !qualified_link.enabled || !matches(metadata) ||
        requested.cadence_hz > source_cadence_ ||
        result.maximum_wire_bytes > source_maximum_frame_ || !qualification_current())) {
      throw std::runtime_error("requested source/profile/cadence lacks current measured qualification");
    }
    const depthd::gateway::SubscriptionRequest request{
        requested.products, encoded ? std::max<std::size_t>(1U, bytes->size() - depthd::wire::kHeaderBytes) : header.payload_bytes,
        depthd::wire::kHardMaximumMetadataBytes, requested.cadence_hz, false, false};
    const auto preference = result.preference == "usb" ? depthd::gateway::TransportPreference::kUsbPreferred :
        (result.preference == "auto" ? depthd::gateway::TransportPreference::kAuto :
         result.preference == "wifi" ? depthd::gateway::TransportPreference::kWifiOnly :
         result.preference == "both" ? depthd::gateway::TransportPreference::kBoth :
         depthd::gateway::TransportPreference::kEthernetPreferred);
    auto admission = traffic_.admit(request, kind, override ? depthd::gateway::TransportPreference::kAuto : preference);
    if (!admission.lease) throw std::runtime_error(admission.reason);
    result.lease = std::move(admission.lease);
    // Allocate the token before taking the memory lock. If its control-block
    // allocation fails, an unarmed deleter must not recursively lock or release
    // a reservation which has not been made.
    auto memory = std::shared_ptr<std::size_t>(new std::size_t(0U),
        [this](std::size_t* value) {
          if (*value != 0U) {
            std::lock_guard release(memory_mutex_);
            memory_bytes_ -= *value;
          }
          delete value;
        });
    {
      std::lock_guard lock(memory_mutex_);
      const auto reservation = std::max(result.maximum_wire_bytes, bytes->retained_size()) * 2U;
      constexpr std::size_t maximum = 64U * 1024U * 1024U;
      if (reservation > maximum - memory_bytes_) throw std::runtime_error("bounded session frame memory exhausted");
      memory_bytes_ += reservation;
      *memory = reservation;
    }
    result.memory = std::move(memory);
    return result;
  }
  [[nodiscard]] bool charge(const Contract& contract, std::size_t bytes) {
    const auto index = static_cast<std::size_t>(contract.transport) - 1U;
    const auto& link = contract.transport == depthd::gateway::TransportKind::kEthernet ? traffic_configuration_.ethernet :
        contract.transport == depthd::gateway::TransportKind::kUsbEthernet ? traffic_configuration_.usb_ethernet : traffic_configuration_.wifi;
    std::lock_guard lock(bandwidth_mutex_);
    auto& bucket = bandwidth_[index];
    const auto now = std::chrono::steady_clock::now();
    const auto bound = static_cast<double>(link.qualified_payload_bytes_per_second);
    bucket.tokens = bucket.at == std::chrono::steady_clock::time_point{} ? bound :
        std::min(bound, bucket.tokens + std::chrono::duration<double>(now - bucket.at).count() * bound);
    bucket.at = now;
    if (static_cast<double>(bytes) > bucket.tokens) return false;
    bucket.tokens -= static_cast<double>(bytes); return true;
  }
  [[nodiscard]] static bool is_wifi(const std::string& interface) {
    return std::filesystem::exists(std::filesystem::path("/sys/class/net") / interface / "wireless");
  }
  [[nodiscard]] bool is_usb(const std::string& interface) const {
    if (interface == usb_interface_) return true;
    std::error_code error;
    const auto path = std::filesystem::canonical(std::filesystem::path("/sys/class/net") / interface / "device", error);
    return !error && path.string().find("/usb") != std::string::npos;
  }
  [[nodiscard]] std::string connection_json(const std::string& interface) const {
    const bool wifi = is_wifi(interface);
    const bool usb = is_usb(interface);
    const auto& link = wifi ? traffic_configuration_.wifi : usb ? traffic_configuration_.usb_ethernet : traffic_configuration_.ethernet;
    const auto qualified = !wifi && (usb ? qualification_configuration_.usb_ethernet.enabled : qualification_configuration_.ethernet.enabled) &&
        (usb ? interface == usb_interface_ : interface == ethernet_interface_) && qualification_current();
    return "{\"interface\":\"" + interface + "\",\"kind\":\"" +
        (wifi ? "wifi" : usb ? "usb_ethernet" : interface == "lo" ? "loopback" : "ethernet") +
        "\",\"qualified_link\":" + (qualified ? "true" : "false") +
        ",\"resource_budget_bytes_per_second\":" + std::to_string(link.qualified_payload_bytes_per_second) +
        ",\"budget_is_measured_capacity\":" + (qualified ? "true" : "false") +
        ",\"recommended_preview\":\"" + (wifi ? "jpeg" : "h264") + "\",\"recommended_cadence_hz\":" + (wifi ? "5" : "10") + "}";
  }
  [[nodiscard]] bool current(const Contract& contract,
                             const FrameSource::Snapshot& frame,
                             const bool encoded) {
    const auto preference = transport_preference(options_);
    if (preference == "off" || preference != contract.preference || !frame.bytes) return false;
    if (encoded) return true;
    // A new subscriber can enlarge the group's backing message without
    // changing this client's selected products. Reserve the full retained
    // owner, including possible rewritten metadata, before queueing views.
    const auto required = std::max(contract.maximum_wire_bytes, frame.bytes->retained_size() +
        depthd::wire::kHardMaximumMetadataBytes + depthd::wire::kHeaderBytes) * 2U;
    {
      std::lock_guard lock(memory_mutex_);
      if (required > *contract.memory) {
        const auto extra = required - *contract.memory;
        if (extra > 64U * 1024U * 1024U - memory_bytes_) return false;
        memory_bytes_ += extra; *contract.memory = required;
      }
    }
    const auto metadata = frame_metadata(*frame.bytes);
    return identity(metadata, contract.products) == contract.identity &&
        (!contract.qualified || (matches(metadata) && qualification_current()));
  }
 private:
  [[nodiscard]] static std::string identity(const Json& metadata,
                                           const depthd::ipc_v2::ProductMask mask) {
    auto result = metadata.get<std::string>("calibration_sha256", "") + ":" +
        metadata.get<std::string>("stereo_applied_revision", "") + ":" +
        metadata.get<std::string>("stereo_requested_revision", "") + ":" +
        metadata.get<std::string>("stereo_configuration_error", "") + ":" +
        metadata.get<std::string>("configuration_epoch", "");
    for (const auto& item : metadata.get_child("products")) {
      const auto& product = item.second;
      const auto name = product.get<std::string>("kind");
      if ((product_mask(name) & mask) == 0U) continue;
      result += ":" + name + ":" + product.get<std::string>("format") + ":" +
          product.get<std::string>("width") + ":" + product.get<std::string>("height") +
          ":" + product.get<std::string>("length");
    }
    return result;
  }
  [[nodiscard]] bool matches(const Json& metadata) const {
    const auto authority = metadata.get<unsigned>("calibration_authority", 0U);
    return !source_revision_.empty() && authority >= 1U && authority <= 2U &&
        metadata.get<std::string>("stereo_applied_revision", "") == source_revision_ &&
        metadata.get<std::string>("stereo_requested_revision", "") == source_revision_ &&
        metadata.get<std::string>("stereo_configuration_error", "").empty() &&
        metadata.get<std::string>("calibration_sha256", "") == source_calibration_ &&
        metadata.get<std::uint64_t>("configuration_epoch", 0U) == source_epoch_;
  }
  [[nodiscard]] bool qualification_current() const {
    try {
      const auto bytes = read_file(options_.qualification_file, 8192U);
      return std::string(bytes.begin(), bytes.end()) == qualification_document_;
    } catch (...) { return false; }
  }
  [[nodiscard]] depthd::gateway::AdmissionConfiguration qualification(const Arguments& options) {
    depthd::gateway::AdmissionConfiguration result;
    result.preference = depthd::gateway::TransportPreference::kBoth;
    std::error_code error;
    if (!std::filesystem::exists(options.qualification_file, error)) return result;
    const auto bytes = read_file(options.qualification_file, 8192U);
    qualification_document_.assign(bytes.begin(), bytes.end());
    const auto document = parse_json(qualification_document_);
    if (!document.get<bool>("enabled", true)) return result;
    if (document.get<unsigned>("schema_version") != 1U ||
        document.get<std::string>("measurement_id").empty()) throw std::invalid_argument("invalid gateway qualification evidence");
    source_revision_ = document.get<std::string>("source.stereo_applied_revision");
    source_calibration_ = document.get<std::string>("source.calibration_sha256");
    source_epoch_ = document.get<std::uint64_t>("source.configuration_epoch");
    source_cadence_ = document.get<double>("source.minimum_cadence_hz");
    source_maximum_frame_ = document.get<std::size_t>("source.maximum_frame_bytes");
    const auto hash = [](const std::string& value) {
      return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
      });
    };
    if (!hash(source_revision_) || !hash(source_calibration_) || source_epoch_ == 0U ||
        !std::isfinite(source_cadence_) || source_cadence_ <= 0.0 || source_cadence_ > 60.0 ||
        source_maximum_frame_ < depthd::wire::kHeaderBytes ||
        source_maximum_frame_ > depthd::wire::kHeaderBytes + depthd::wire::kHardMaximumMetadataBytes +
            depthd::wire::kHardMaximumPayloadBytes) throw std::invalid_argument("invalid qualified source bounds");
    const auto transport = [&](const char* name, std::string& interface) {
      depthd::gateway::QualifiedTransport value;
      if (!document.get<bool>(std::string(name) + ".enabled", false)) return value;
      interface = document.get<std::string>(std::string(name) + ".interface");
      if (interface.empty() || interface.size() > 15U || interface.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos) {
        throw std::invalid_argument("invalid qualified interface");
      }
      value.enabled = true;
      value.qualified_payload_bytes_per_second = document.get<std::uint64_t>(std::string(name) + ".payload_bytes_per_second");
      value.maximum_frame_bytes = document.get<std::uint64_t>(std::string(name) + ".maximum_frame_bytes");
      value.maximum_sessions = document.get<std::size_t>(std::string(name) + ".maximum_sessions");
      return value;
    };
    result.ethernet = transport("ethernet", ethernet_interface_);
    result.usb_ethernet = transport("usb_ethernet", usb_interface_);
    if (ethernet_interface_ == usb_interface_) throw std::invalid_argument("qualified interfaces must differ");
    return result;
  }
  [[nodiscard]] depthd::gateway::AdmissionConfiguration engineering(const Arguments& options) {
    depthd::gateway::AdmissionConfiguration result;
    result.preference = depthd::gateway::TransportPreference::kAuto;
    result.ethernet = result.usb_ethernet = {true, true, options.engineering_byte_budget,
        depthd::wire::kHeaderBytes + depthd::wire::kHardMaximumMetadataBytes +
            depthd::wire::kHardMaximumPayloadBytes, options.maximum_sessions};
    result.wifi = {true, true, options.wifi_byte_budget, result.ethernet.maximum_frame_bytes, options.maximum_sessions};
    return result;
  }
  [[nodiscard]] depthd::gateway::AdmissionConfiguration effective_configuration(const Arguments& options) {
    auto result = engineering(options);
    if (qualification_configuration_.ethernet.enabled) result.ethernet = qualification_configuration_.ethernet;
    if (qualification_configuration_.usb_ethernet.enabled) result.usb_ethernet = qualification_configuration_.usb_ethernet;
    return result;
  }
  const Arguments& options_;
  std::string qualification_document_;
  std::string ethernet_interface_{"eth0"}, usb_interface_{"usb0"};
  std::string source_revision_, source_calibration_;
  std::uint64_t source_epoch_{};
  double source_cadence_{};
  std::size_t source_maximum_frame_{};
  depthd::gateway::AdmissionConfiguration qualification_configuration_, traffic_configuration_;
  depthd::gateway::AdmissionController traffic_;
  struct Bucket { double tokens{}; std::chrono::steady_clock::time_point at{}; };
  std::array<Bucket, 3> bandwidth_{};
  std::mutex bandwidth_mutex_;
  std::mutex memory_mutex_;
  std::size_t memory_bytes_{};
};

// Duplicates remain owned until deregistration. Shutdown and deregistration
// share a lock, so descriptor recycling cannot target an unrelated connection.
class ConnectionRegistry final {
 public:
  class Registration final {
   public:
    Registration(ConnectionRegistry& registry, const int descriptor)
        : registry_(registry), descriptor_(::dup(descriptor)) {
      if (descriptor_ < 0) throw std::runtime_error("cannot track client socket");
      std::lock_guard lock(registry_.mutex_);
      registry_.descriptors_.insert(descriptor_);
      if (registry_.stopped_) ::shutdown(descriptor_, SHUT_RDWR);
    }
    ~Registration() {
      std::lock_guard lock(registry_.mutex_);
      registry_.descriptors_.erase(descriptor_);
      ::close(descriptor_);
    }
   private:
    ConnectionRegistry& registry_;
    int descriptor_;
  };
  void shutdown() noexcept {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    for (const auto descriptor : descriptors_) ::shutdown(descriptor, SHUT_RDWR);
  }
 private:
  std::mutex mutex_;
  std::unordered_set<int> descriptors_;
  bool stopped_{};
};

class StreamSession final {
 public:
  StreamSession(beast::ssl_stream<beast::tcp_stream> stream,
                FrameSource& source, StreamAdmission& admission,
                StreamAdmission::Contract contract, Subscription requested,
                const bool encoded, FrameSource::DemandLease demand)
      : socket_(std::move(stream)), source_(source), admission_(admission),
        contract_(std::move(contract)), requested_(requested), encoded_(encoded), demand_(std::move(demand)),
        queue_(contract_.maximum_wire_bytes), tick_(socket_.get_executor()),
        write_deadline_(socket_.get_executor()) {}
  ~StreamSession() { finish(); }
  void run(const http::request<http::string_body>& request, net::io_context& io) {
    io_ = &io;
    beast::get_lowest_layer(socket_).expires_never();
    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::server);
    timeout.handshake_timeout = std::chrono::seconds(5);
    timeout.idle_timeout = std::chrono::seconds(15);
    timeout.keep_alive_pings = true;
    socket_.set_option(timeout);
    socket_.set_option(websocket::stream_base::decorator([this](websocket::response_type& response) {
      response.set("X-Nuvo-Stream-Qualification", contract_.label);
      response.set("X-Nuvo-Stream-Cadence-Hz", std::to_string(requested_.cadence_hz));
      response.set("X-Nuvo-Stream-Cadence-Semantics", contract_.qualified ? "exact" : "maximum-best-effort");
    }));
    socket_.read_message_max(4096U);
    socket_.binary(true);
    socket_.accept(request);
    subscriber_ = source_.subscribe([this, &io] {
      if (notification_pending_.exchange(true)) return;
      net::post(io, [this] {
        notification_pending_.store(false);
        update();
      });
    });
    read();
    update();
    io.run();
    queue_.close_incomplete();
  }
 private:
  void finish() noexcept {
    if (finished_) return;
    finished_ = true;
    source_.unsubscribe(subscriber_);
    demand_.reset();
    queue_.close_incomplete();
    beast::error_code ignored;
    tick_.cancel(ignored);
    write_deadline_.cancel(ignored);
    auto& socket = beast::get_lowest_layer(socket_).socket();
    socket.cancel(ignored);
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    if (io_) io_->stop();
  }
  void read() {
    socket_.async_read(input_, [this](const beast::error_code& error, std::size_t) {
      // Ping/pong and close are processed by Beast while this read is active.
      // The stream is server-to-client; unsolicited data closes the session.
      if (error || input_.size() != 0U) { finish(); return; }
      if (!finished_) read();
    });
  }
  void send() {
    if (finished_) return;
    auto bytes = queue_.begin_buffered_transmission();
    if (!bytes) return;
    write_deadline_.expires_after(std::chrono::seconds(5));
    write_deadline_.async_wait([this](const beast::error_code& error) {
      if (!error) finish();
    });
    auto buffers = std::make_shared<std::vector<net::const_buffer>>();
    for (const auto part : bytes->parts()) buffers->emplace_back(part.data(), part.size());
    socket_.async_write(*buffers,
        [this, bytes, buffers](const beast::error_code& error, const std::size_t count) {
          beast::error_code ignored;
          write_deadline_.cancel(ignored);
          if (finished_) return;
          if (error || count != bytes->size()) { finish(); return; }
          queue_.note_bytes_sent(count);
          queue_.complete_transmission();
          send();
        });
  }
  void update() {
    if (finished_) return;
    try {
      if (stopping.load() || !source_.enabled(encoded_)) { finish(); return; }
      const auto now = std::chrono::steady_clock::now();
      auto frame = source_.latest(encoded_, requested_.products);
      if (!frame.bytes) {
        if (unavailable_since_ == std::chrono::steady_clock::time_point{}) unavailable_since_ = now;
        if (now - unavailable_since_ >= std::chrono::seconds(3)) { finish(); return; }
        tick_.expires_after(std::chrono::milliseconds(100));
        tick_.async_wait([this](const beast::error_code& error) { if (!error) update(); });
        return;
      }
      unavailable_since_ = {};
      if (!admission_.current(contract_, frame, encoded_)) { finish(); return; }
      if (frame.generation != previous_ && now >= next_send_) {
        previous_ = frame.generation;
        if (!encoded_) frame = source_.selected(frame, requested_.products);
        if (!admission_.charge(contract_, frame.bytes->size())) {
          if (contract_.qualified) { finish(); return; }
        } else {
        if (queue_.offer_buffered(frame.bytes) == depthd::gateway::OfferResult::kOversize) {
          finish(); return;
        }
        send();
        }
        next_send_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / requested_.cadence_hz));
      }
      auto delay = std::chrono::milliseconds(250);
      if (frame.generation != previous_ && next_send_ > now) {
        delay = std::min(delay, std::max(std::chrono::milliseconds(1),
            std::chrono::duration_cast<std::chrono::milliseconds>(next_send_ - now)));
      }
      tick_.expires_after(delay);
      tick_.async_wait([this](const beast::error_code& error) { if (!error) update(); });
    } catch (...) { finish(); }
  }
  websocket::stream<beast::ssl_stream<beast::tcp_stream>> socket_;
  FrameSource& source_;
  StreamAdmission& admission_;
  StreamAdmission::Contract contract_;
  Subscription requested_;
  bool encoded_{};
  FrameSource::DemandLease demand_;
  std::chrono::steady_clock::time_point unavailable_since_{};
  depthd::gateway::SessionQueue queue_;
  net::steady_timer tick_, write_deadline_;
  beast::flat_buffer input_;
  std::uint64_t previous_{};
  std::uint64_t subscriber_{};
  std::chrono::steady_clock::time_point next_send_{};
  bool finished_{};
  net::io_context* io_{};
  std::atomic_bool notification_pending_{};
};

void await_clients(const std::atomic_size_t& active) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (active.load() != 0U) {
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "nuvo-gateway: clients did not stop within shutdown deadline\n";
      std::_Exit(2);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

[[nodiscard]] Response route(const http::request<http::string_body>& request,
                             const Arguments& options, const std::string& claim,
                             Sessions& sessions, FrameSource& source,
                             const FrameSource::Snapshot* admitted_snapshot = nullptr,
                             const std::string& connection = "{}") {
  const auto full_target = std::string_view(request.target());
  const auto path = full_target.substr(0, full_target.find('?'));
  const auto target = path == "/api/v1/frame/latest" || path == "/api/v1/encoded/latest" ? path : full_target;
  if (target.size() > 2048U || !valid_origin(request)) {
    return text_response(http::status::forbidden, "application/json",
                         "{\"error\":\"origin rejected\"}\n", request.version());
  }
  if (request.method() == http::verb::post && target == "/api/v1/session") {
    // Reserve capacity before consuming a single-use setup code.
    auto session = sessions.create();
    if (!session) {
      return text_response(http::status::service_unavailable, "application/json",
                           "{\"error\":\"session capacity reached\"}\n", request.version());
    }
    bool accepted = authenticated(request, claim, sessions);
    bool needs_password = false;
    if (!accepted) {
      try {
        std::string secret;
        if (!request.body().empty() && request.body().size() <= 2048U) {
          const auto body = parse_json(request.body());
          if (body.size() == 1U && body.front().first == "secret" && body.front().second.empty()) {
            secret = body.get<std::string>("secret");
          }
        } else {
          const std::string authorization(request[http::field::authorization]);
          if (authorization.starts_with("Bearer ")) secret = authorization.substr(7);
        }
        if (!secret.empty() && secret.size() <= 1024U) {
          if (equal_secret(secret, claim)) {
            accepted = true;
            needs_password = needs_owner_password(options);
          } else {
            const auto result = setup_agent(options, "setup.authenticate", "secret", secret);
            accepted = result.get<bool>("authenticated", false);
            needs_password = result.get<bool>("needs_owner_password", false);
          }
        }
      } catch (const std::exception&) { accepted = false; }
    } else {
      needs_password = needs_owner_password(options);
    }
    if (!accepted) {
      sessions.remove(session->session);
      return text_response(http::status::unauthorized, "application/json",
                           "{\"error\":\"invalid device password or setup code\"}\n",
                           request.version());
    }
    const auto body = "{\"role\":\"admin\",\"csrf\":\"" +
                      session->csrf + "\",\"needs_owner_password\":" + (needs_password ? "true" : "false") + "}\n";
    auto result = text_response(http::status::ok, "application/json", body,
                                request.version());
    result.set(http::field::set_cookie,
               "nuvo_session=" + session->session +
                   "; Path=/; Secure; HttpOnly; SameSite=Strict");
    return result;
  }
  if (target.starts_with("/api/") &&
      !authenticated(request, claim, sessions)) {
    return text_response(http::status::unauthorized, "application/json",
                         "{\"error\":\"authentication required\"}\n",
                         request.version());
  }
  if (request.method() == http::verb::get && target == "/api/v1/session") {
    const auto csrf = sessions.csrf_for(cookie_value(
        request[http::field::cookie], "nuvo_session"));
    if (!csrf) {
      return text_response(http::status::unauthorized, "application/json",
                           "{\"error\":\"browser session required\"}\n",
                           request.version());
    }
    return text_response(http::status::ok, "application/json",
                         "{\"role\":\"admin\",\"csrf\":\"" + *csrf +
                             "\",\"needs_owner_password\":" + (needs_owner_password(options) ? "true" : "false") + "}\n",
                         request.version());
  }
  if (request.method() == http::verb::get && target == "/api/v1/setup/status") {
    try {
      return response(http::status::ok, "application/json",
                      system_agent(options, "{\"operation\":\"setup.status\"}", 10), request.version());
    } catch (const std::exception&) {
      return text_response(http::status::service_unavailable, "application/json",
                           "{\"error\":\"setup status unavailable\"}\n", request.version());
    }
  }
  if (request.method() != http::verb::get &&
      request.method() != http::verb::head && !valid_csrf(request, sessions)) {
    return text_response(http::status::forbidden, "application/json",
                         "{\"error\":\"CSRF token rejected\"}\n",
                         request.version());
  }
  if (request.method() == http::verb::post && target == "/api/v1/setup/owner") {
    try {
      const auto body = parse_json(request.body());
      if (body.size() != 1U || body.front().first != "password" || !body.front().second.empty()) {
        throw std::invalid_argument("invalid owner password request");
      }
      const auto password = body.get<std::string>("password");
      if (password.size() < 12U || password.size() > 1024U) throw std::invalid_argument("invalid password length");
      (void)setup_agent(options, "setup.owner", "password", password);
      return text_response(http::status::ok, "application/json", "{\"owner_configured\":true}\n", request.version());
    } catch (const std::exception&) {
      return text_response(http::status::bad_request, "application/json", "{\"error\":\"could not set password; use 12 through 256 characters\"}\n", request.version());
    }
  }
  if (request.method() == http::verb::get && target == "/api/v1/transport") {
    return text_response(http::status::ok, "application/json",
                         "{\"preference\":\"" +
                             transport_preference(options) + "\"}\n",
                         request.version());
  }
  if (target == "/api/v1/stereo" || target == "/api/v1/exposure" ||
      target == "/api/v1/encoding") {
    const auto path = target == "/api/v1/stereo" ? options.stereo_control_file
                      : target == "/api/v1/exposure" ? options.exposure_control_file
                                                     : options.encoding_config_file;
    const auto kind = target == "/api/v1/stereo" ? "stereo" :
                      target == "/api/v1/exposure" ? "exposure" : "encoding";
    try {
      if (request.method() == http::verb::get) {
        auto bytes = read_file(path, 4096U);
        const auto revision = depthd::revision::sha256_hex(std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto result = response(http::status::ok, "application/json",
                               std::move(bytes), request.version());
        result.set(http::field::etag, "\"" + revision + "\"");
        result.set("X-Nuvo-Configuration-State", "requested");
        return result;
      }
      if (request.method() == http::verb::post) {
        const auto match = request[http::field::if_match];
        if (match.empty()) {
          return text_response(static_cast<http::status>(428), "application/json",
              "{\"error\":\"If-Match required; load configuration before saving\"}\n",
              request.version());
        }
        const std::string expected(match);
        if (expected.size() != 66U || expected.front() != '"' ||
            expected.back() != '"' ||
            !std::all_of(expected.begin() + 1, expected.end() - 1,
                [](const char value) { return (value >= '0' && value <= '9') ||
                                             (value >= 'a' && value <= 'f'); }) ||
            request.body().size() > 4096U) {
          return text_response(http::status::bad_request, "application/json",
              "{\"error\":\"invalid configuration or strong If-Match ETag\"}\n",
              request.version());
        }
        const auto current = read_file(path, 4096U);
        if (expected != "\"" + depthd::revision::sha256_hex(std::string_view(
            reinterpret_cast<const char*>(current.data()), current.size())) + "\"") {
          return text_response(http::status::precondition_failed, "application/json",
              "{\"error\":\"configuration changed; reload before saving\"}\n",
              request.version());
        }
        // This check is repeated while holding the privileged writer's lock.
        // The receipt contains the exact saved JSON; a later writer cannot
        // cause this response to acknowledge another request's configuration.
        const auto reply = system_agent(options,
            "{\"operation\":\"" + std::string(kind) + ".update\",\"expected_revision\":\"\\\"" +
            expected.substr(1U, 64U) + "\\\"\",\"configuration\":" + request.body() + "}");
        boost::property_tree::ptree document;
        std::istringstream input(std::string(reply.begin(), reply.end()));
        boost::property_tree::read_json(input, document);
        if (document.get<std::string>("status") != "ok") {
          return response(document.get<std::string>("error_code", "") == "precondition_failed"
                              ? http::status::precondition_failed : http::status::bad_request,
                          "application/json", reply, request.version());
        }
        const auto saved = document.get<std::string>("result.configuration_json");
        const auto saved_revision = document.get<std::string>("result.requested_revision");
        if (depthd::revision::sha256_hex(saved) != saved_revision) {
          throw std::runtime_error("invalid configuration receipt");
        }
        auto result = text_response(http::status::accepted, "application/json", saved,
                                    request.version());
        result.set(http::field::etag, "\"" + saved_revision + "\"");
        result.set("X-Nuvo-Configuration-State", "pending");
        return result;
      }
    } catch (const std::exception&) {
      return text_response(http::status::service_unavailable, "application/json",
                           "{\"error\":\"configuration control unavailable\"}\n",
                           request.version());
    }
  }
  if (request.method() == http::verb::get) {
    if (const auto session = calibration_session_query(
            target, "/api/v1/calibration")) {
      try {
        auto result = system_agent(
            options, "{\"operation\":\"calibration.status\",\"session_id\":\"" +
                         *session + "\"}");
        return response(agent_succeeded(result) ? http::status::ok
                                                : http::status::bad_request,
                        "application/json", std::move(result), request.version());
      } catch (const std::exception&) {
        return text_response(http::status::service_unavailable,
                             "application/json",
                             "{\"error\":\"calibration status unavailable\"}\n",
                             request.version());
      }
    }
    for (const std::string_view side : {"left", "right"}) {
      const auto path = "/api/v1/calibration/preview/" + std::string(side);
      if (const auto session = calibration_session_query(target, path)) {
        try {
          const auto image = options.calibration_state_root /
              "calibration-sessions" / *session /
              ("preview-" + std::string(side) + ".jpg");
          return response(http::status::ok, "image/jpeg",
                          read_file(image, 8U * 1024U * 1024U),
                          request.version());
        } catch (const std::exception&) {
          return text_response(http::status::not_found, "application/json",
                               "{\"error\":\"calibration preview unavailable\"}\n",
                               request.version());
        }
      }
    }
  }
  if (request.method() == http::verb::post &&
      target == "/api/v1/calibration/session") {
    if (request.body().size() > 6144U) {
      return text_response(http::status::bad_request, "application/json",
                           "{\"error\":\"calibration configuration too large\"}\n",
                           request.version());
    }
    try {
      auto result = system_agent(
          options, "{\"operation\":\"calibration.create\",\"configuration\":" +
                       request.body() + "}");
      return response(agent_succeeded(result) ? http::status::ok
                                              : http::status::bad_request,
                      "application/json", std::move(result), request.version());
    } catch (const std::exception&) {
      return text_response(http::status::bad_request, "application/json",
                           "{\"error\":\"calibration session request failed\"}\n",
                           request.version());
    }
  }
  if (request.method() == http::verb::post &&
      (target == "/api/v1/calibration/capture" ||
       target == "/api/v1/calibration/solve" ||
       target == "/api/v1/calibration/activate" ||
       target == "/api/v1/calibration/rollback")) {
    std::string_view operation = "calibration.rollback";
    if (target.ends_with("capture")) operation = "calibration.capture";
    else if (target.ends_with("solve")) operation = "calibration.solve";
    else if (target.ends_with("activate")) operation = "calibration.activate";
    try {
      auto result = system_agent(
          options, calibration_agent_request(operation, request.body()));
      return response(agent_succeeded(result) ? http::status::ok
                                              : http::status::bad_request,
                      "application/json", std::move(result), request.version());
    } catch (const std::exception&) {
      return text_response(http::status::bad_request, "application/json",
                           "{\"error\":\"calibration operation failed\"}\n",
                           request.version());
    }
  }
  if (request.method() == http::verb::post &&
      target == "/api/v1/transport/stage") {
    try {
      const auto preference = request_preference(request.body());
      if (preference == "off") {
        throw std::invalid_argument(
            "turning all data transports off requires physical nuvoctl confirmation");
      }
      auto result = system_agent(options,
                                 "{\"operation\":\"transport.stage\","
                                 "\"preference\":\"" + preference + "\"}");
      const auto status = agent_succeeded(result) ? http::status::ok
                                                  : http::status::bad_request;
      return response(status, "application/json", std::move(result),
                      request.version());
    } catch (const std::exception&) {
      return text_response(http::status::bad_request, "application/json",
                           "{\"error\":\"control request failed\"}\n",
                           request.version());
    }
  }
  if (request.method() == http::verb::post &&
      (target == "/api/v1/transport/confirm" ||
       target == "/api/v1/transport/rollback")) {
    const auto operation = target.ends_with("confirm")
                               ? "transport.confirm"
                               : "transport.rollback";
    try {
      auto result = system_agent(options, "{\"operation\":\"" +
                                              std::string(operation) + "\"}");
      const auto status = agent_succeeded(result) ? http::status::ok
                                                  : http::status::bad_request;
      return response(status, "application/json", std::move(result),
                      request.version());
    } catch (const std::exception&) {
      return text_response(http::status::bad_request, "application/json",
                           "{\"error\":\"control request failed\"}\n",
                           request.version());
    }
  }
  if (request.method() == http::verb::get && target == "/api/v1/health") {
    try {
      return response(http::status::ok, "application/json",
                      read_file(options.status_file, 64U * 1024U),
                      request.version());
    } catch (const std::exception&) {
      return text_response(http::status::service_unavailable, "application/json",
                           "{\"schema_version\":1,\"status\":\"camera_unavailable\"}\n",
                           request.version());
    }
  }
  if (request.method() == http::verb::get && target == "/api/v1/capabilities") {
    const auto consumers = source.consumers();
    auto capabilities = std::string(
        "{\"schema_version\":1,\"products\":[\"left_rectified\","
        "\"right_rectified\",\"disparity\",\"depth\",\"confidence\","
        "\"validity\",\"left_mono\",\"right_mono\",\"left_rgb\",\"right_rgb\","
        "\"left_rectified_rgb\",\"right_rectified_rgb\",\"rectified_stereo\",\"rectified_stereo_rgb\"],\"depth_unit\":\"metres\","
        "\"capture_controls\":[\"manual_exposure\",\"stereo_auto_exposure\"],"
        "\"calibration\":{\"target\":\"charuco\",\"raw_solver_frames\":true,"
        "\"encoded_positioning_preview\":true,\"holdout_validation\":true},"
        "\"transports\":[\"ethernet\",\"usb_ethernet\",\"wifi\"],"
        "\"stream_admission\":{\"exact_requires_measured_qualification\":true,"
        "\"default_stream\":\"best_effort\",\"engineering_interface\":\"") +
        options.engineering_interface +
        "\",\"raw_maximum_wire_bytes\":16777216,\"encoded_maximum_wire_bytes\":4194304,"
        "\"session_memory_budget_bytes\":67108864,\"demand_driven\":true},"
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
        "\"encoded_streams\":[\"h264\",\"jpeg\"],"
        "\"encoded_sources\":[\"left_color\",\"right_color\","
        "\"left_rectified\",\"right_rectified\",\"disparity\",\"depth\","
        "\"left_mono\",\"right_mono\",\"left_rgb\",\"right_rgb\",\"left_rectified_rgb\","
        "\"right_rectified_rgb\",\"rectified_stereo\",\"rectified_stereo_rgb\"],"
#endif
        "\"connection\":" + connection + ",\"active_consumers\":{\"raw\":" +
        std::to_string(consumers.first) + ",\"encoded\":" + std::to_string(consumers.second) + "}," +
        "\"transport_preference\":\"" + transport_preference(options) +
        "\",\"protocol\":\"NuvoStream/1.0\"}\n";
    return text_response(
        http::status::ok, "application/json",
        capabilities,
        request.version());
  }
  if (request.method() == http::verb::get && target == "/api/v1/frame/latest") {
    if (!source.enabled(false)) {
      return text_response(http::status::service_unavailable, "application/json",
                           "{\"error\":\"data transport is disabled\"}\n",
                           request.version());
    }
    try {
      // Snapshot bodies may only copy the exact immutable bytes covered by a
      // live admission reservation. Looking up latest again could select a
      // larger publication after reserving capacity for the previous one.
      if (!admitted_snapshot || !admitted_snapshot->bytes) {
        throw std::runtime_error("no admitted local frame available");
      }
      const auto& frame = *admitted_snapshot;
      return response(http::status::ok, "application/vnd.nuvo.frame-set",
                      frame.bytes->copy(),
                      request.version());
    } catch (const std::exception&) {
      return text_response(http::status::service_unavailable, "application/json",
                           "{\"error\":\"no frame available\"}\n",
                           request.version());
    }
  }
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
  if (request.method() == http::verb::get &&
      target == "/api/v1/encoded/latest") {
    if (!source.enabled(true)) {
      return text_response(http::status::service_unavailable,
                           "application/json",
                           "{\"error\":\"encoded stream is disabled\"}\n",
                           request.version());
    }
    try {
      if (!admitted_snapshot || !admitted_snapshot->bytes) {
        throw std::runtime_error("no admitted encoded frame available");
      }
      const auto& frame = *admitted_snapshot;
      return response(http::status::ok,
                      "application/vnd.nuvo.encoded-image",
                      frame.bytes->copy(),
                      request.version());
    } catch (const std::exception&) {
      return text_response(http::status::service_unavailable,
                           "application/json",
                           "{\"error\":\"no encoded frame available\"}\n",
                           request.version());
    }
  }
#endif
  if (request.method() != http::verb::get) {
    return text_response(http::status::method_not_allowed, "application/json",
                         "{\"error\":\"method not allowed\"}\n",
                         request.version());
  }
  std::filesystem::path file;
  std::string_view type;
  if (target == "/" || target == "/index.html") {
    file = options.web_root / "index.html";
    type = "text/html; charset=utf-8";
  } else if (target == "/app.js" || target == "/device-settings.js" ||
             target == "/frame-codec.js") {
    file = options.web_root / std::string(target.substr(1));
    type = "text/javascript; charset=utf-8";
  } else if (target == "/styles.css") {
    file = options.web_root / std::string(target.substr(1));
    type = "text/css; charset=utf-8";
  } else if (target == "/assets/fonts/hanken-grotesk-400.ttf" ||
             target == "/assets/fonts/hanken-grotesk-500.ttf" ||
             target == "/assets/fonts/hanken-grotesk-600.ttf") {
    file = options.web_root / std::string(target.substr(1));
    type = "font/ttf";
  } else if (target == "/depth-profiles.json") {
    file = options.web_root / "depth-profiles.json";
    type = "application/json";
  } else {
    return text_response(http::status::not_found, "application/json",
                         "{\"error\":\"not found\"}\n", request.version());
  }
  try {
    return response(http::status::ok, type, read_file(file, 2U * 1024U * 1024U),
                    request.version());
  } catch (const std::exception&) {
    return text_response(http::status::not_found, "application/json",
                         "{\"error\":\"UI asset missing\"}\n", request.version());
  }
}

void serve(tcp::socket socket, ssl::context& context, const Arguments& options,
           const std::string& claim, Sessions& sessions,
           std::atomic_size_t& active, FrameSource& source,
           ConnectionRegistry& connections, StreamAdmission& admission,
           std::atomic_size_t& idle_http) noexcept {
  struct Done { std::atomic_size_t& value; ~Done() { --value; } } done{active};
  struct IdleLease {
    std::atomic_size_t& count; bool held{};
    void release() { if (held) { --count; held = false; } }
    ~IdleLease() { release(); }
    bool acquire(std::size_t limit) {
      auto current = count.load();
      while (current < limit) {
        if (count.compare_exchange_weak(current, current + 1U)) { held = true; return true; }
      }
      return false;
    }
  } idle{idle_http};
  try {
    ConnectionRegistry::Registration registration(connections, socket.native_handle());
    const auto interface = receiving_interface(socket);
    const timeval timeout{5, 0};
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
                     &timeout, sizeof(timeout)) != 0) {
      throw std::runtime_error("cannot set client socket deadline");
    }
    net::io_context client_io;
    const auto protocol = socket.local_endpoint().protocol();
    tcp::socket client_socket(client_io, protocol, socket.release());
    beast::ssl_stream<beast::tcp_stream> stream(std::move(client_socket), context);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
    stream.handshake(ssl::stream_base::server);
    beast::flat_buffer buffer;
    const auto connected_at = std::chrono::steady_clock::now();
    for (std::size_t request_count = 0; request_count < 32U && !stopping.load(); ++request_count) {
    http::request_parser<http::string_body> parser;
    parser.body_limit(64U * 1024U);
    http::read(stream, buffer, parser);
    idle.release();
    const auto websocket_target = std::string_view(parser.get().target());
    const auto websocket_path = websocket_target.substr(0U, websocket_target.find('?'));
    if (websocket::is_upgrade(parser.get()) &&
        (websocket_path == "/api/v1/stream" ||
         websocket_path == "/api/v1/encoded/stream")) {
      if (!valid_origin(parser.get()) ||
          !authenticated(parser.get(), claim, sessions)) {
        auto denied = text_response(
            http::status::unauthorized, "application/json",
            "{\"error\":\"authenticated same-origin session required\"}\n",
            parser.get().version());
        http::write(stream, denied);
        beast::error_code ignored;
        stream.shutdown(ignored);
        return;
      }
      bool upgraded{};
      try {
        const bool encoded = websocket_path == "/api/v1/encoded/stream";
        auto requested = subscription(websocket_target, encoded);
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
        if (encoded && websocket_target.find("cadence_hz=") == std::string_view::npos) {
          requested.cadence_hz = encoding_configuration(options).cadence_hz;
        }
#endif
        admission.preflight(requested, interface, encoded);
        auto demand = source.acquire(encoded, requested.products, requested.cadence_hz);
        auto frame = source.enabled(encoded) ? source.wait_ready(encoded, requested.products) : FrameSource::Snapshot{};
        if (!frame.bytes) {
          auto denied = text_response(http::status::service_unavailable, "application/json",
              "{\"error\":\"no complete source frame available\"}\n", parser.get().version());
          http::write(stream, denied);
          return;
        }
        if (!encoded) frame = source.selected(frame, requested.products);
        auto contract = admission.admit(requested, interface, frame.bytes, encoded);
        upgraded = true;
        StreamSession session(std::move(stream), source, admission, std::move(contract),
                              requested, encoded, std::move(demand));
        session.run(parser.get(), client_io);
      } catch (const std::invalid_argument&) {
        if (upgraded) return;
        auto denied = text_response(http::status::bad_request, "application/json",
            "{\"error\":\"invalid stream product/cadence subscription\"}\n", parser.get().version());
        http::write(stream, denied);
      } catch (const std::exception& error) {
        // Admission exceptions occur before the TLS stream is moved. A failed
        // upgraded connection is handled by its session's bounded close path.
        if (!upgraded && beast::get_lowest_layer(stream).socket().is_open()) {
          auto denied = text_response(http::status::conflict, "application/json",
              std::string("{\"error\":\"") + error.what() + "\"}\n", parser.get().version());
          http::write(stream, denied);
        }
      }
      return;
    }
    if (parser.get().method() == http::verb::get && valid_origin(parser.get()) && authenticated(parser.get(), claim, sessions) &&
        (websocket_path == "/api/v1/frame/latest" || websocket_path == "/api/v1/encoded/latest")) {
      const bool encoded = websocket_path == "/api/v1/encoded/latest";
        bool response_started{};
        try {
          auto requested = subscription(websocket_target, encoded);
          // A one-shot response does not reserve the plain stream's 10 Hz
          // contract. Its complete bytes still consume the shared token budget,
          // which limits repeated callers independently of this demand hint.
          if (websocket_target.find("cadence_hz=") == std::string_view::npos) requested.cadence_hz = 1.0;
          admission.preflight(requested, interface, encoded);
          auto demand = source.acquire(encoded, requested.products, requested.cadence_hz);
          auto frame = source.enabled(encoded) ? source.wait_ready(encoded, requested.products) : FrameSource::Snapshot{};
          if (!frame.bytes) {
            auto unavailable = text_response(http::status::service_unavailable, "application/json",
                "{\"error\":\"no fresh matching source frame available\"}\n", parser.get().version());
            http::write(stream, unavailable); return;
          }
          if (!encoded) frame = source.selected(frame, requested.products);
          auto contract = admission.admit(requested, interface, frame.bytes, encoded);
          if (!admission.charge(contract, frame.bytes->size())) throw std::runtime_error("output byte budget exhausted");
          auto result = route(parser.get(), options, claim, sessions, source, &frame);
          result.set("X-Nuvo-Stream-Qualification", contract.label);
          response_started = true;
          http::write(stream, result);
          return;
        } catch (const std::exception& error) {
          // A partially sent response must close; a second HTTP response would
          // append unrelated bytes to the declared snapshot body.
          if (response_started) return;
          auto denied = text_response(dynamic_cast<const std::invalid_argument*>(&error) ? http::status::bad_request : http::status::conflict, "application/json",
              "{\"error\":\"receiving interface or bounded data capacity unavailable\"}\n", parser.get().version());
          http::write(stream, denied);
          return;
        }
    }
    auto result = route(parser.get(), options, claim, sessions, source, nullptr, admission.connection_json(interface));
    const bool reuse = parser.get().keep_alive() && request_count + 1U < 32U &&
        std::chrono::steady_clock::now() - connected_at < std::chrono::seconds(30) &&
        idle.acquire(std::min<std::size_t>(4U, options.maximum_sessions / 4U));
    result.keep_alive(reuse);
    if (reuse) result.set(http::field::keep_alive, "timeout=5, max=" + std::to_string(31U - request_count));
    http::write(stream, result);
    if (!reuse) break;
    const timeval idle_timeout{5, 0};
    if (::setsockopt(beast::get_lowest_layer(stream).socket().native_handle(), SOL_SOCKET,
                     SO_RCVTIMEO, &idle_timeout, sizeof(idle_timeout)) != 0)
      throw std::runtime_error("cannot set keepalive idle deadline");
    }
    idle.release();
    beast::error_code ignored;
    stream.shutdown(ignored);
  } catch (const std::exception&) {
    // Per-client protocol and disconnect failures do not terminate the service.
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = arguments(argc, argv);
    const auto claim = token(options.token_file);
    net::io_context io;
    ssl::context context(ssl::context::tls_server);
    context.set_options(ssl::context::default_workarounds |
                        ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                        ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1 |
                        ssl::context::single_dh_use);
    if (SSL_CTX_set_min_proto_version(context.native_handle(),
                                      TLS1_2_VERSION) != 1) {
      throw std::runtime_error("cannot set TLS minimum version");
    }
    context.use_certificate_chain_file(options.certificate.string());
    context.use_private_key_file(options.private_key.string(), ssl::context::pem);
    tcp::acceptor acceptor(io, {net::ip::make_address(options.address), options.port});
    acceptor.non_blocking(true);
    Sessions sessions(options.maximum_sessions);
    std::atomic_size_t active{};
    std::atomic_size_t idle_http{};
    stopping.store(false);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    FrameSource source(options);
    StreamAdmission admission(options);
    ConnectionRegistry connections;
    struct ClientsGuard {
      FrameSource& source;
      ConnectionRegistry& connections;
      std::atomic_size_t& active;
      ~ClientsGuard() {
        stopping.store(true);
        connections.shutdown();
        source.stop();
        await_clients(active);
      }
    } clients_guard{source, connections, active};
    std::cout << "nuvo-gateway listening on https://" << options.address << ':'
              << options.port << '\n';
    while (!stopping.load()) {
      beast::error_code error;
      auto socket = acceptor.accept(error);
      if (error == net::error::would_block || error == net::error::try_again) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      if (error) throw beast::system_error(error);
      if (active.load() >= options.maximum_sessions) {
        beast::error_code ignored;
        socket.close(ignored);
        continue;
      }
      ++active;
      try {
        std::thread(serve, std::move(socket), std::ref(context),
                  std::cref(options), std::cref(claim), std::ref(sessions),
                  std::ref(active), std::ref(source), std::ref(connections),
                  std::ref(admission), std::ref(idle_http)).detach();
      } catch (...) {
        --active;
        throw;
      }
    }
    acceptor.close();
    connections.shutdown();
    source.stop();
    await_clients(active);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "nuvo-gateway: " << error.what() << '\n';
    return 2;
  }
}
