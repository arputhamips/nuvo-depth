#include <ares.h>
#include <poll.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "nuvo/client.hpp"
#if defined(__unix__)
#include <sys/stat.h>
#endif

namespace nuvo::client {
namespace {
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using Stream = beast::ssl_stream<beast::tcp_stream>;
using Error = beast::error_code;
struct DnsState {
  std::map<ares_socket_t, short> sockets;
  bool done{};
  int status{ARES_ECANCELLED};
  std::vector<tcp::endpoint> endpoints;
};
nlohmann::json control_document(const std::string& body) {
  try {
    return nlohmann::json::parse(
        body, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
          if (depth > 16)
            throw std::invalid_argument("control nesting exceeds bound");
          return true;
        });
  } catch (...) {
    throw std::runtime_error("invalid bounded Nuvo control response");
  }
}
class Https final : public Source {
 public:
  explicit Https(HttpsOptions options)
      : options_(std::move(options)),
        tls_(ssl::context::tls_client),
        ws_(io_, tls_) {
    validate(options_.subscription);
    if (options_.timeout.count() < 100 ||
        options_.timeout > std::chrono::minutes(2))
      throw std::invalid_argument("HTTPS timeout must be 100..120000 ms");
    // Deliberately reject credentials, query strings, fragments and base paths.
    std::smatch parts;
    const std::regex url(
        R"(^https://([A-Za-z0-9._-]+|\[[0-9A-Fa-f:]+\])(?::([0-9]{1,5}))?/?$)");
    if (!std::regex_match(options_.url, parts, url))
      throw std::invalid_argument(
          "HTTPS URL must contain a host and optional port only");
    host_ = parts[1];
    port_ = parts[2].matched ? parts[2].str() : "443";
    if (std::stoul(port_) == 0 || std::stoul(port_) > 65535)
      throw std::invalid_argument("invalid HTTPS port");
    authority_ = host_ + ":" + port_;
    if (host_.front() == '[') host_ = host_.substr(1, host_.size() - 2);
    tls_.set_default_verify_paths();
    if (!options_.ca_file.empty())
      tls_.load_verify_file(options_.ca_file.string());
    tls_.set_verify_mode(ssl::verify_peer);
#if defined(__unix__)
    struct stat status {};
    if (::stat(options_.credential_file.c_str(), &status) != 0 ||
        !S_ISREG(status.st_mode) || (status.st_mode & 0077) != 0)
      throw std::invalid_argument(
          "credential file must be regular and restricted to its owner (0600)");
#endif
    std::ifstream file(options_.credential_file, std::ios::binary);
    if (!file) throw std::invalid_argument("cannot read credential file");
    std::string token(258, '\0');
    file.read(token.data(), 258);
    token.resize(static_cast<std::size_t>(file.gcount()));
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r'))
      token.pop_back();
    if (token.size() < 32 || token.size() > 256 ||
        token.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJ"
                                "KLMNOPQRSTUVWXYZ_-") != std::string::npos)
      throw std::invalid_argument("invalid credential file content");
    token_ = std::move(token);
  }
  ~Https() override {
    // Drain cancellation before the bounded best-effort revocation. The HTTP
    // channel is independent of WebSocket data and never exposes cookie values.
    try {
      io_.restart();
      io_.poll();
      if (!csrf_.empty()) {
        options_.timeout = std::chrono::milliseconds(250);
        // Ordinary gateway HTTP keep-alive expires independently of the data
        // stream. Revoke through a fresh channel rather than a stale idle TLS
        // connection after a long acquisition session.
        close_http();
        auto request = make_request(http::verb::delete_, "/api/v1/session");
        request.set(http::field::cookie, cookie_);
        request.set("X-Nuvo-CSRF", csrf_);
        (void)exchange(request, true);
      }
    } catch (...) {
    }
    close_sockets();
  }
  void cancel() noexcept override {
    stopped_ = true;
    try {
      net::post(io_, [this] { interrupt_io(); });
    } catch (...) {
    }
  }
  std::optional<Frame> next() override {
    if (stopped_) return {};
    try {
      if (!connected_) connect();
      // flat_buffer is contiguous. Retain its allocation with the Frame rather
      // than allocating, zeroing and copying the entire pixel payload again.
      auto buffer = std::make_shared<beast::flat_buffer>(maximum_wire_bytes);
      operation([&](auto done) {
        ws_.async_read(*buffer, [done](Error e, std::size_t) { done(e); });
      });
      if (!ws_.got_binary())
        throw std::runtime_error("Nuvo stream returned a nonbinary message");
      const auto bytes = buffer->data();
      return decode_frame(
          {static_cast<const std::byte*>(bytes.data()), bytes.size()}, buffer);
    } catch (...) {
      // A partial handshake/read cannot be retried on this stream.
      stopped_ = true;
      close_sockets();
      throw;
    }
  }

 private:
  using Completion = std::function<void(Error)>;
  void operation(const std::function<void(Completion)>& start,
                 bool cleanup = false) {
    if (stopped_ && !cleanup)
      throw std::runtime_error("HTTPS acquisition cancelled");
    io_.restart();
    Error result;
    bool complete = false, expired = false;
    net::steady_timer timer(io_);
    timer.expires_after(options_.timeout);
    timer.async_wait([&](Error e) {
      if (!e && !complete) {
        expired = true;
        interrupt_io();
      }
    });
    start([&](Error error) {
      result = error;
      complete = true;
      timer.cancel();
    });
    io_.run();
    if (expired) throw std::runtime_error("Nuvo HTTPS operation timed out");
    if (result || (!cleanup && stopped_))
      throw std::runtime_error("Nuvo HTTPS connection failed or was cancelled");
  }
  void close_sockets() noexcept {
    Error ignored;
    for (auto* s : {http_.get(), &ws_.next_layer()}) {
      if (!s) continue;
      beast::get_lowest_layer(*s).socket().cancel(ignored);
      beast::get_lowest_layer(*s).socket().close(ignored);
    }
  }
  void close_http() noexcept {
    if (!http_) return;
    Error ignored;
    beast::get_lowest_layer(*http_).socket().close(ignored);
    http_.reset();
  }
  void interrupt_io() noexcept {
    Error ignored;
    if (http_) beast::get_lowest_layer(*http_).socket().cancel(ignored);
    beast::get_lowest_layer(ws_).socket().cancel(ignored);
    beast::get_lowest_layer(ws_).socket().close(ignored);
  }
  void connect_tls(Stream& stream, bool cleanup = false) {
    resolve(cleanup);
    operation(
        [&](auto done) {
          beast::get_lowest_layer(stream).async_connect(
              endpoints_, [done](Error e, const auto&) { done(e); });
        },
        cleanup);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str()))
      throw std::runtime_error("TLS server name setup failed");
    stream.set_verify_mode(ssl::verify_peer);
    stream.set_verify_callback(ssl::host_name_verification(host_));
    operation(
        [&](auto done) {
          stream.async_handshake(ssl::stream_base::client, done);
        },
        cleanup);
  }
  void resolve(bool cleanup) {
    if (stopped_ && !cleanup)
      throw std::runtime_error("HTTPS acquisition cancelled");
    Error error;
    const auto address = net::ip::make_address(
        options_.connect_address.empty() ? host_ : options_.connect_address,
        error);
    if (!error) {
      endpoints_ = {tcp::endpoint(
          address, static_cast<unsigned short>(std::stoul(port_)))};
      return;
    }
    if (!options_.connect_address.empty())
      throw std::invalid_argument(
          "connect_address must be a numeric IPv4 or IPv6 address");
    static const bool initialized = [] {
      if (ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS)
        throw std::runtime_error("cannot initialize DNS library");
      return true;
    }();
    (void)initialized;
    DnsState state;
    struct Channel {
      ares_channel value{};
      ~Channel() {
        if (value) ares_destroy(value);
      }
    } channel;
    ares_options config{};
    config.sock_state_cb = [](void* arg, ares_socket_t fd, int readable,
                              int writable) {
      auto& sockets = static_cast<DnsState*>(arg)->sockets;
      if (!readable && !writable)
        sockets.erase(fd);
      else
        sockets[fd] = static_cast<short>((readable ? POLLIN : 0) |
                                         (writable ? POLLOUT : 0));
    };
    config.sock_state_cb_data = &state;
    if (ares_init_options(&channel.value, &config, ARES_OPT_SOCK_STATE_CB) !=
        ARES_SUCCESS)
      throw std::runtime_error("cannot initialize DNS channel");
    ares_addrinfo_hints hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = ARES_AI_NOSORT | ARES_AI_NUMERICSERV;
    ares_getaddrinfo(
        channel.value, host_.c_str(), port_.c_str(), &hints,
        [](void* arg, int status, int, ares_addrinfo* result) {
          auto& output = *static_cast<DnsState*>(arg);
          output.status = status;
          output.done = true;
          try {
            if (status == ARES_SUCCESS && result)
              for (auto* node = result->nodes; node; node = node->ai_next) {
                tcp::endpoint endpoint;
                if ((node->ai_family == AF_INET ||
                     node->ai_family == AF_INET6) &&
                    static_cast<std::size_t>(node->ai_addrlen) <=
                        endpoint.capacity()) {
                  std::memcpy(endpoint.data(), node->ai_addr, node->ai_addrlen);
                  endpoint.resize(node->ai_addrlen);
                  output.endpoints.push_back(endpoint);
                }
              }
          } catch (...) {
            output.status = ARES_ENOMEM;
            output.endpoints.clear();
          }
          if (result) ares_freeaddrinfo(result);
        },
        &state);
    const auto deadline = std::chrono::steady_clock::now() + options_.timeout;
    while (!state.done) {
      if ((stopped_ && !cleanup) ||
          std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Nuvo DNS lookup timed out or was cancelled");
      std::vector<pollfd> descriptors;
      for (const auto& [fd, events] : state.sockets)
        descriptors.push_back({fd, events, 0});
      const auto ready = ::poll(descriptors.data(), descriptors.size(), 20);
      if (ready < 0 && errno != EINTR)
        throw std::runtime_error("Nuvo DNS poll failed");
      for (const auto& descriptor : descriptors)
        if (descriptor.revents && !state.done)
          ares_process_fd(
              channel.value,
              (descriptor.revents & (POLLIN | POLLERR | POLLHUP))
                  ? descriptor.fd
                  : ARES_SOCKET_BAD,
              (descriptor.revents & POLLOUT) ? descriptor.fd : ARES_SOCKET_BAD);
      if (!state.done)
        ares_process_fd(channel.value, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
    }
    if (state.status != ARES_SUCCESS || state.endpoints.empty())
      throw std::runtime_error(
          "Nuvo hostname resolution failed; use connect_address for externally "
          "resolved mDNS names");
    endpoints_ = state.endpoints;
  }
  http::request<http::string_body> make_request(http::verb method,
                                                const std::string& path) {
    http::request<http::string_body> r{method, path, 11};
    r.set(http::field::host, authority_);
    r.set(http::field::user_agent, "nuvo-depth-client/0.1");
    r.keep_alive(true);
    return r;
  }
  http::response<http::string_body> exchange(
      http::request<http::string_body>& request, bool cleanup = false) {
    if (!http_) {
      http_ = std::make_unique<Stream>(io_, tls_);
      connect_tls(*http_, cleanup);
    }
    operation(
        [&](auto done) {
          http::async_write(*http_, request,
                            [done](Error e, std::size_t) { done(e); });
        },
        cleanup);
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(65536);
    parser.header_limit(16384);
    operation(
        [&](auto done) {
          http::async_read(*http_, buffer, parser,
                           [done](Error e, std::size_t) { done(e); });
        },
        cleanup);
    auto response = parser.release();
    if (!response.keep_alive()) {
      Error ignored;
      beast::get_lowest_layer(*http_).socket().close(ignored);
      http_.reset();
    }
    return response;
  }
  void connect() {
    auto claim = make_request(http::verb::post, "/api/v1/session");
    claim.set(http::field::authorization, "Bearer " + token_);
    claim.prepare_payload();
    const auto response = exchange(claim);
    std::fill(token_.begin(), token_.end(), '\0');
    token_.clear();
    if (response.result() != http::status::ok)
      throw std::runtime_error("Nuvo session rejected (HTTP " +
                               std::to_string(response.result_int()) + ")");
    const auto json = control_document(response.body());
    csrf_ = json.at("csrf").get<std::string>();
    const auto cookie = std::string(response[http::field::set_cookie]);
    const auto end = cookie.find(';');
    cookie_ = cookie.substr(0, end);
    if (!std::regex_match(cookie_, std::regex("nuvo_session=[a-f0-9]{64}")) ||
        !std::regex_match(csrf_, std::regex("[a-f0-9]{64}")))
      throw std::runtime_error("invalid Nuvo session response");
    auto capabilities = make_request(http::verb::get, "/api/v1/capabilities");
    capabilities.set(http::field::cookie, cookie_);
    const auto cap = exchange(capabilities);
    if (cap.result() != http::status::ok)
      throw std::runtime_error("Nuvo capabilities request rejected");
    const auto document = control_document(cap.body());
    if (document.value("protocol", std::string{}) != "NuvoStream/1.0")
      throw std::runtime_error("unsupported Nuvo protocol");
    for (const auto& product : options_.subscription.products) {
      bool found = false;
      for (const auto& p : document.at("products"))
        if (p == product) found = true;
      if (!found)
        throw std::runtime_error("device lacks requested product: " + product);
    }
    close_http();  // Leave the gateway's bounded idle HTTP pool available.
    connect_tls(ws_.next_layer());
    ws_.read_message_max(maximum_wire_bytes);
    ws_.auto_fragment(false);
    ws_.set_option(
        websocket::stream_base::decorator([this](websocket::request_type& r) {
          r.set(http::field::cookie, cookie_);
        }));
    std::ostringstream target;
    target.imbue(std::locale::classic());
    target << "/api/v1/stream?products=";
    bool first = true;
    for (const auto& p : options_.subscription.products) {
      if (!first) target << ',';
      target << p;
      first = false;
    }
    target << "&cadence_hz=" << options_.subscription.cadence_hz
           << "&delivery=" << options_.subscription.delivery;
    operation([&](auto done) {
      ws_.async_handshake(authority_, target.str(), done);
    });
    connected_ = true;
  }
  HttpsOptions options_;
  net::io_context io_;
  ssl::context tls_;
  std::unique_ptr<Stream> http_;
  websocket::stream<Stream> ws_;
  std::vector<tcp::endpoint> endpoints_;
  std::string host_, port_, authority_, token_, cookie_, csrf_;
  std::atomic_bool stopped_{};
  bool connected_{};
};
}  // namespace
std::unique_ptr<Source> https_source(const HttpsOptions& options) {
  return std::make_unique<Https>(options);
}
}  // namespace nuvo::client
