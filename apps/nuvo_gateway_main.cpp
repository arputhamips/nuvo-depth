#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "depthd/local_ipc.hpp"
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
  std::filesystem::path encoded_file{
      "/run/nuvo-depth/snapshots/latest.nve"};
  std::filesystem::path encoding_config_file{
      "/etc/nuvo-depth/encoding.json"};
  std::filesystem::path stereo_control_file{
      "/etc/nuvo-depth/stereo.json"};
  std::filesystem::path transport_file{"/etc/nuvo-depth/transport.json"};
  std::filesystem::path system_agent_socket{
      "/run/nuvo-depth/system-agent.sock"};
  std::size_t maximum_sessions{16U};
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
    else if (option == "--snapshot-file") result.snapshot_file = value;
    else if (option == "--encoded-file") result.encoded_file = value;
    else if (option == "--encoding-config-file") result.encoding_config_file = value;
    else if (option == "--stereo-control-file") result.stereo_control_file = value;
    else if (option == "--transport-file") result.transport_file = value;
    else if (option == "--system-agent-socket") result.system_agent_socket = value;
    else if (option == "--maximum-sessions") result.maximum_sessions = number(value, option);
    else throw std::invalid_argument("unknown option: " + std::string(option));
  }
  if (result.certificate.empty() || result.private_key.empty() ||
      result.token_file.empty() || result.maximum_sessions > 128U) {
    throw std::invalid_argument(
        "--certificate, --private-key, and --token-file are required");
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
    const auto bytes = read_file(options.transport_file, 4096U);
    const std::string document(bytes.begin(), bytes.end());
    for (const std::string value : {"ethernet", "usb", "both", "off"}) {
      if (document.find("\"preference\"") != std::string::npos &&
          document.find("\"" + value + "\"") != std::string::npos) {
        return value;
      }
    }
  } catch (const std::exception&) {
  }
  return "ethernet";
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
      value != "off") {
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
    const Arguments& options, const std::string_view request) {
  auto connection = depthd::ipc_v2::connect_seqpacket(
      options.system_agent_socket);
  const auto bytes = std::span(
      reinterpret_cast<const std::byte*>(request.data()), request.size());
  connection.send_packet(bytes);
  const auto response = connection.receive_packet(64U * 1024U, 0U);
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

#if defined(DEPTHD_WITH_IMAGE_ENCODER)
[[nodiscard]] depthd::encoding::Configuration encoding_configuration(
    const Arguments& options) {
  depthd::encoding::Configuration fallback;
  fallback.mode = depthd::encoding::StreamMode::kRawOnly;
  return depthd::encoding::load_configuration(options.encoding_config_file,
                                               fallback);
}
#endif

void stream_frames(beast::ssl_stream<beast::tcp_stream> stream,
                   const http::request<http::string_body>& request,
                   const Arguments& options,
                   const bool encoded) {
  websocket::stream<beast::ssl_stream<beast::tcp_stream>> socket(
      std::move(stream));
  beast::get_lowest_layer(socket).expires_never();
  socket.set_option(websocket::stream_base::timeout::suggested(
      beast::role_type::server));
  socket.binary(true);
  socket.accept(request);
  std::filesystem::file_time_type previous{};
  while (!stopping.load()) {
    try {
      if (transport_preference(options) == "off") {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      const auto& path = encoded ? options.encoded_file : options.snapshot_file;
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
      const auto encoding = encoding_configuration(options);
      if ((encoded && !depthd::encoding::encoded_enabled(encoding)) ||
          (!encoded && !depthd::encoding::raw_enabled(encoding))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
#else
      if (encoded) break;
#endif
      const auto modified = std::filesystem::last_write_time(path);
      if (modified != previous) {
        auto frame = read_file(path, encoded ? 33U * 1024U * 1024U
                                             : 65U * 1024U * 1024U);
        socket.write(net::buffer(frame));
        previous = modified;
      }
    } catch (const std::filesystem::filesystem_error&) {
      // Camera startup can precede the first complete snapshot.
    }
    // A 20 ms poll can miss a 60 Hz access unit even on an idle link. Poll
    // frequently enough to preserve predictive H.264 continuity; the browser
    // still detects and recovers from any unavoidable transport gap.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  beast::error_code ignored;
  socket.close(websocket::close_code::normal, ignored);
}

[[nodiscard]] Response route(const http::request<http::string_body>& request,
                             const Arguments& options, const std::string& claim,
                             Sessions& sessions) {
  const auto target = std::string_view(request.target());
  if (target.size() > 2048U || !valid_origin(request)) {
    return text_response(http::status::forbidden, "application/json",
                         "{\"error\":\"origin rejected\"}\n", request.version());
  }
  if (request.method() == http::verb::post && target == "/api/v1/session") {
    if (!authenticated(request, claim, sessions)) {
      return text_response(http::status::unauthorized, "application/json",
                           "{\"error\":\"invalid claim token\"}\n",
                           request.version());
    }
    auto session = sessions.create();
    if (!session) {
      return text_response(http::status::service_unavailable, "application/json",
                           "{\"error\":\"session capacity reached\"}\n",
                           request.version());
    }
    const auto body = "{\"role\":\"admin\",\"csrf\":\"" +
                      session->csrf + "\"}\n";
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
                             "\"}\n",
                         request.version());
  }
  if (request.method() != http::verb::get &&
      request.method() != http::verb::head && !valid_csrf(request, sessions)) {
    return text_response(http::status::forbidden, "application/json",
                         "{\"error\":\"CSRF token rejected\"}\n",
                         request.version());
  }
  if (request.method() == http::verb::get && target == "/api/v1/transport") {
    return text_response(http::status::ok, "application/json",
                         "{\"preference\":\"" +
                             transport_preference(options) + "\"}\n",
                         request.version());
  }
  if (request.method() == http::verb::get && target == "/api/v1/stereo") {
    try {
      const auto bytes = read_file(options.stereo_control_file, 4096U);
      return response(http::status::ok, "application/json", bytes,
                      request.version());
    } catch (const std::exception&) {
      return text_response(http::status::service_unavailable,
                           "application/json",
                           "{\"error\":\"stereo controls unavailable\"}\n",
                           request.version());
    }
  }
  if (request.method() == http::verb::post && target == "/api/v1/stereo") {
    if (request.body().size() > 4096U) {
      return text_response(http::status::bad_request, "application/json",
                           "{\"error\":\"stereo request too large\"}\n",
                           request.version());
    }
    try {
      const auto result = system_agent(
          options, "{\"operation\":\"stereo.update\",\"configuration\":" +
                       request.body() + "}");
      if (!agent_succeeded(result)) {
        return response(http::status::bad_request, "application/json", result,
                        request.version());
      }
      const auto bytes = read_file(options.stereo_control_file, 4096U);
      return response(http::status::ok, "application/json", bytes,
                      request.version());
    } catch (const std::exception&) {
      return text_response(http::status::service_unavailable,
                           "application/json",
                           "{\"error\":\"stereo control request failed\"}\n",
                           request.version());
    }
  }
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
  if (request.method() == http::verb::get && target == "/api/v1/encoding") {
    try {
      const auto body =
          depthd::encoding::configuration_json(encoding_configuration(options)) +
          "\n";
      return text_response(http::status::ok, "application/json", body,
                           request.version());
    } catch (const std::exception&) {
      return text_response(http::status::service_unavailable,
                           "application/json",
                           "{\"error\":\"encoding configuration unavailable\"}\n",
                           request.version());
    }
  }
  if (request.method() == http::verb::post && target == "/api/v1/encoding") {
    try {
      const auto updated = depthd::encoding::parse_configuration(request.body());
      const auto body = depthd::encoding::configuration_json(updated);
      auto result = system_agent(
          options, "{\"operation\":\"encoding.update\",\"configuration\":" +
                       body + "}");
      if (!agent_succeeded(result)) {
        throw std::runtime_error("encoding control agent rejected update");
      }
      return text_response(http::status::ok, "application/json", body + "\n",
                           request.version());
    } catch (const std::exception&) {
      return text_response(http::status::bad_request, "application/json",
                           "{\"error\":\"encoding control request failed\"}\n",
                           request.version());
    }
  }
#endif
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
    auto capabilities = std::string(
        "{\"schema_version\":1,\"products\":[\"left_rectified\","
        "\"right_rectified\",\"disparity\",\"depth\",\"confidence\","
        "\"validity\"],\"depth_unit\":\"metres\","
        "\"transports\":[\"ethernet\",\"wifi\",\"usb_ethernet\"],"
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
        "\"encoded_streams\":[\"h264\",\"jpeg\"],"
#endif
        "\"transport_preference\":\"") + transport_preference(options) +
        "\",\"protocol\":\"NuvoStream/1.0\"}\n";
    return text_response(
        http::status::ok, "application/json",
        capabilities,
        request.version());
  }
  if (request.method() == http::verb::get && target == "/api/v1/frame/latest") {
    if (transport_preference(options) == "off"
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
        || !depthd::encoding::raw_enabled(encoding_configuration(options))
#endif
    ) {
      return text_response(http::status::service_unavailable, "application/json",
                           "{\"error\":\"data transport is disabled\"}\n",
                           request.version());
    }
    try {
      return response(http::status::ok, "application/vnd.nuvo.frame-set",
                      read_file(options.snapshot_file, 65U * 1024U * 1024U),
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
    if (transport_preference(options) == "off" ||
        !depthd::encoding::encoded_enabled(encoding_configuration(options))) {
      return text_response(http::status::service_unavailable,
                           "application/json",
                           "{\"error\":\"encoded stream is disabled\"}\n",
                           request.version());
    }
    try {
      return response(http::status::ok,
                      "application/vnd.nuvo.encoded-image",
                      read_file(options.encoded_file, 33U * 1024U * 1024U),
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
  } else if (target == "/app.js") {
    file = options.web_root / "app.js";
    type = "text/javascript; charset=utf-8";
  } else if (target == "/styles.css") {
    file = options.web_root / "styles.css";
    type = "text/css; charset=utf-8";
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
           std::atomic_size_t& active) noexcept {
  struct Done { std::atomic_size_t& value; ~Done() { --value; } } done{active};
  try {
    beast::ssl_stream<beast::tcp_stream> stream(std::move(socket), context);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
    stream.handshake(ssl::stream_base::server);
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    parser.body_limit(64U * 1024U);
    http::read(stream, buffer, parser);
    const auto websocket_target = std::string_view(parser.get().target());
    if (websocket::is_upgrade(parser.get()) &&
        (websocket_target == "/api/v1/stream" ||
         websocket_target == "/api/v1/encoded/stream")) {
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
      stream_frames(std::move(stream), parser.get(), options,
                    websocket_target == "/api/v1/encoded/stream");
      return;
    }
    auto result = route(parser.get(), options, claim, sessions);
    http::write(stream, result);
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
    stopping.store(false);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
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
      std::thread(serve, std::move(socket), std::ref(context),
                  std::cref(options), std::cref(claim), std::ref(sessions),
                  std::ref(active)).detach();
    }
    acceptor.close();
    while (active.load() != 0U) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "nuvo-gateway: " << error.what() << '\n';
    return 2;
  }
}
