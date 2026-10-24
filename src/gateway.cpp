#include "depthd/gateway.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace depthd::gateway {
namespace {

struct Utilization {
  std::uint64_t reserved{};
  std::size_t sessions{};
};

[[nodiscard]] const QualifiedTransport& transport_config(
    const AdmissionConfiguration& configuration, const TransportKind kind) {
  return kind == TransportKind::kEthernet ? configuration.ethernet
      : (kind == TransportKind::kUsbEthernet ? configuration.usb_ethernet : configuration.wifi);
}

[[nodiscard]] Utilization& utilization(AdmissionState& state,
                                       TransportKind kind);

[[nodiscard]] std::uint64_t required_rate(const SubscriptionRequest& request) {
  const auto frame_bytes = static_cast<long double>(wire::kHeaderBytes) +
                           request.metadata_bytes + request.frame_payload_bytes;
  const auto rate = std::ceil(frame_bytes * request.cadence_hz);
  if (!std::isfinite(rate) || rate <= 0.0L ||
      rate > static_cast<long double>(
                 std::numeric_limits<std::uint64_t>::max())) {
    throw std::invalid_argument("subscription bandwidth calculation overflows");
  }
  return static_cast<std::uint64_t>(rate);
}

[[nodiscard]] bool can_admit(const QualifiedTransport& config,
                             const Utilization& used,
                             const SubscriptionRequest& request,
                             const std::uint64_t rate) {
  const auto frame_bytes = wire::kHeaderBytes + request.metadata_bytes +
                           request.frame_payload_bytes;
  return config.enabled && config.tls_enabled && config.maximum_sessions != 0U &&
         frame_bytes <= config.maximum_frame_bytes &&
         used.sessions < config.maximum_sessions &&
         rate <= config.qualified_payload_bytes_per_second -
                     std::min(config.qualified_payload_bytes_per_second,
                              used.reserved);
}

[[nodiscard]] std::vector<TransportKind> preference_order(
    const TransportPreference preference, const std::optional<bool> wifi_enabled = {}) {
  std::vector<TransportKind> result;
  switch (preference) {
    case TransportPreference::kEthernetPreferred:
      result = {TransportKind::kEthernet, TransportKind::kUsbEthernet}; break;
    case TransportPreference::kUsbPreferred:
      result = {TransportKind::kUsbEthernet, TransportKind::kEthernet}; break;
    case TransportPreference::kBoth:
      result = {TransportKind::kEthernet, TransportKind::kUsbEthernet}; break;
    case TransportPreference::kOff:
      return {};
    case TransportPreference::kAuto:
      result = {TransportKind::kEthernet, TransportKind::kUsbEthernet, TransportKind::kWifi}; break;
    case TransportPreference::kWifiOnly:
      result = {TransportKind::kWifi}; break;
    default:
      return {};
  }
  if (wifi_enabled) {
    std::erase(result, TransportKind::kWifi);
    if (*wifi_enabled) result.push_back(TransportKind::kWifi);
  }
  return result;
}

}  // namespace

std::string_view network_path(const bool loopback, const bool wireless,
    const std::string_view kernel_device, const std::string_view kernel_network,
    const bool bound_gadget_interface) {
  if (loopback) return "other";
  if (wireless) return "wifi";
  if (bound_gadget_interface || kernel_device.ends_with("/gadget") ||
      kernel_network.find("/gadget/net/") != std::string_view::npos) return "usb_device";
  if (kernel_device.find("/usb") != std::string_view::npos) return "usb_host_adapter";
  return kernel_device.empty() ? "other" : "ethernet";
}

bool transport_permitted(const TransportPreference preference,
                         const TransportKind transport,
                         const std::optional<bool> wifi_enabled,
                         const bool encoded) {
  const auto order = preference_order(preference, encoded ? std::optional(true) : wifi_enabled);
  return std::find(order.begin(), order.end(), transport) != order.end();
}

struct AdmissionState {
  AdmissionConfiguration configuration{};
  mutable std::mutex mutex;
  Utilization ethernet{};
  Utilization usb{};
  Utilization wifi{};
  AdmissionStats stats{};
};

namespace {

Utilization& utilization(AdmissionState& state, const TransportKind kind) {
  return kind == TransportKind::kEthernet ? state.ethernet
      : (kind == TransportKind::kUsbEthernet ? state.usb : state.wifi);
}

}  // namespace

struct AdmissionLease::Impl {
  std::shared_ptr<AdmissionState> state;
  TransportKind transport{TransportKind::kEthernet};
  std::uint64_t rate{};
};

AdmissionLease::AdmissionLease(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
AdmissionLease::~AdmissionLease() noexcept { reset(); }
AdmissionLease::AdmissionLease(AdmissionLease&&) noexcept = default;
AdmissionLease& AdmissionLease::operator=(AdmissionLease&& other) noexcept {
  if (this != &other) {
    reset();
    impl_ = std::move(other.impl_);
  }
  return *this;
}
AdmissionLease::operator bool() const noexcept { return impl_ != nullptr; }
TransportKind AdmissionLease::transport() const {
  if (!impl_) {
    throw std::logic_error("gateway admission lease is inactive");
  }
  return impl_->transport;
}
std::uint64_t AdmissionLease::reserved_bytes_per_second() const {
  if (!impl_) {
    throw std::logic_error("gateway admission lease is inactive");
  }
  return impl_->rate;
}
void AdmissionLease::reset() noexcept {
  auto impl = std::move(impl_);
  if (!impl) {
    return;
  }
  std::lock_guard lock(impl->state->mutex);
  auto& used = utilization(*impl->state, impl->transport);
  used.reserved = impl->rate <= used.reserved ? used.reserved - impl->rate : 0U;
  if (used.sessions != 0U) {
    --used.sessions;
  }
}

AdmissionController::AdmissionController(AdmissionConfiguration configuration)
    : state_(std::make_shared<AdmissionState>()) {
  validate(configuration);
  state_->configuration = configuration;
}

AdmissionResult AdmissionController::admit(const SubscriptionRequest& request) {
  validate(request);
  const auto rate = required_rate(request);
  AdmissionResult result;
  std::lock_guard lock(state_->mutex);
  auto order = preference_order(state_->configuration.preference,
      request.encoded ? std::optional(true) : state_->configuration.wifi_enabled);
  if (state_->configuration.preference == TransportPreference::kBoth &&
      order.size() >= 2U) {
    const auto ethernet_capacity =
        state_->configuration.ethernet.qualified_payload_bytes_per_second;
    const auto usb_capacity =
        state_->configuration.usb_ethernet.qualified_payload_bytes_per_second;
    const auto ethernet_remaining =
        ethernet_capacity - std::min(ethernet_capacity,
                                     state_->ethernet.reserved);
    const auto usb_remaining =
        usb_capacity - std::min(usb_capacity, state_->usb.reserved);
    if (usb_remaining > ethernet_remaining) {
      std::swap(order[0], order[1]);
    }
  }
  for (const auto kind : order) {
    const auto& config = transport_config(state_->configuration, kind);
    auto& used = utilization(*state_, kind);
    if (config.enabled && config.tls_enabled &&
        wire::kHeaderBytes + request.metadata_bytes +
                request.frame_payload_bytes <=
            config.maximum_frame_bytes &&
        std::find(result.compatible_alternatives.begin(),
                  result.compatible_alternatives.end(), kind) ==
            result.compatible_alternatives.end()) {
      result.compatible_alternatives.push_back(kind);
    }
    if (!can_admit(config, used, request, rate)) {
      continue;
    }
    used.reserved += rate;
    ++used.sessions;
    ++state_->stats.admitted;
    auto impl = std::make_unique<AdmissionLease::Impl>();
    impl->state = state_;
    impl->transport = kind;
    impl->rate = rate;
    result.lease.emplace(AdmissionLease(std::move(impl)));
    result.reason = "admitted exact requested contract";
    return result;
  }
  ++state_->stats.rejected;
  result.reason = order.empty()
                      ? "all data transports are disabled by policy"
                      : "requested frame size/cadence exceeds qualified or available capacity";
  return result;
}

AdmissionResult AdmissionController::admit(
    const SubscriptionRequest& request, const TransportKind connected_transport,
    const std::optional<TransportPreference> preference,
    const std::optional<bool> wifi_enabled,
    const TransportMask available_alternatives) {
  validate(request);
  const auto rate = required_rate(request);
  AdmissionResult result;
  std::lock_guard lock(state_->mutex);
  const auto policy = preference.value_or(state_->configuration.preference);
  const auto wifi = wifi_enabled ? wifi_enabled : state_->configuration.wifi_enabled;
  const auto permitted_wifi = request.encoded ? std::optional(true) : wifi;
  const auto order = preference_order(policy, permitted_wifi);
  const bool preferred_available = policy != TransportPreference::kBoth &&
      policy != TransportPreference::kAuto && !order.empty() &&
      !(connected_transport == TransportKind::kWifi && permitted_wifi.value_or(false)) &&
      order.front() != connected_transport &&
      (available_alternatives & transport_bit(order.front())) != 0U &&
      can_admit(transport_config(state_->configuration, order.front()),
                utilization(*state_, order.front()), request, rate);
  if (!preferred_available &&
      std::find(order.begin(), order.end(), connected_transport) != order.end()) {
    const auto& config = transport_config(state_->configuration, connected_transport);
    auto& used = utilization(*state_, connected_transport);
    if (can_admit(config, used, request, rate)) {
      used.reserved += rate;
      ++used.sessions;
      ++state_->stats.admitted;
      auto impl = std::make_unique<AdmissionLease::Impl>();
      impl->state = state_;
      impl->transport = connected_transport;
      impl->rate = rate;
      result.lease.emplace(AdmissionLease(std::move(impl)));
  result.reason = "admitted requested rate on connected interface";
      return result;
    }
  }
  for (const auto kind : order) {
    if (kind != connected_transport && (available_alternatives & transport_bit(kind)) != 0U &&
        can_admit(transport_config(state_->configuration, kind),
                  utilization(*state_, kind), request, rate)) {
      result.compatible_alternatives.push_back(kind);
    }
  }
  ++state_->stats.rejected;
  result.reason = order.empty() ? "all data transports are disabled by policy"
      : "connected transport has no available configured capacity; reduce the requested rate or reconnect on a compatible interface";
  return result;
}

AdmissionStats AdmissionController::stats() const noexcept {
  std::lock_guard lock(state_->mutex);
  auto result = state_->stats;
  result.ethernet_reserved_bytes_per_second = state_->ethernet.reserved;
  result.usb_reserved_bytes_per_second = state_->usb.reserved;
  result.wifi_reserved_bytes_per_second = state_->wifi.reserved;
  result.ethernet_sessions = state_->ethernet.sessions;
  result.usb_sessions = state_->usb.sessions;
  result.wifi_sessions = state_->wifi.sessions;
  return result;
}

SessionQueue::SessionQueue(const std::size_t maximum_wire_bytes)
    : maximum_wire_bytes_(maximum_wire_bytes) {
  if (maximum_wire_bytes_ < wire::kHeaderBytes ||
      maximum_wire_bytes_ > wire::kHeaderBytes +
                                wire::kHardMaximumMetadataBytes +
                                wire::kHardMaximumPayloadBytes) {
    throw std::invalid_argument("gateway session message bound is invalid");
  }
  stats_.open = true;
}

OfferResult SessionQueue::offer(std::shared_ptr<const wire::Message> message) {
  if (!message) {
    throw std::invalid_argument("gateway cannot offer a null message");
  }
  wire::validate(*message);
  if (message->wire_bytes() > maximum_wire_bytes_) {
    return OfferResult::kOversize;
  }
  std::lock_guard lock(mutex_);
  if (!open_) {
    return OfferResult::kClosed;
  }
  ++stats_.offered;
  const auto replaced = static_cast<bool>(pending_) || static_cast<bool>(pending_bytes_);
  pending_bytes_.reset();
  pending_ = std::move(message);
  if (replaced) {
    ++stats_.pending_replaced;
  }
  stats_.pending_messages = 1U;
  return replaced ? OfferResult::kReplacedPendingComplete
                  : OfferResult::kAccepted;
}

OfferResult SessionQueue::offer_serialized(
    std::shared_ptr<const std::vector<std::byte>> bytes) {
  if (!bytes) throw std::invalid_argument("gateway cannot offer null serialized data");
  return offer_buffered(std::make_shared<const SerializedBytes>(std::move(bytes)));
}

OfferResult SessionQueue::offer_buffered(std::shared_ptr<const SerializedBytes> bytes) {
  if (!bytes || bytes->empty()) {
    throw std::invalid_argument("gateway cannot offer empty serialized data");
  }
  if (bytes->size() > maximum_wire_bytes_) return OfferResult::kOversize;
  std::lock_guard lock(mutex_);
  if (!open_) return OfferResult::kClosed;
  ++stats_.offered;
  const auto replaced = static_cast<bool>(pending_) || static_cast<bool>(pending_bytes_);
  pending_.reset();
  pending_bytes_ = std::move(bytes);
  if (replaced) ++stats_.pending_replaced;
  stats_.pending_messages = 1U;
  return replaced ? OfferResult::kReplacedPendingComplete : OfferResult::kAccepted;
}

std::shared_ptr<const wire::Message> SessionQueue::begin_transmission() {
  std::lock_guard lock(mutex_);
  if (!open_ || in_flight_ || in_flight_bytes_ || !pending_) {
    return {};
  }
  in_flight_ = std::move(pending_);
  pending_.reset();
  in_flight_bytes_sent_ = 0U;
  stats_.pending_messages = 0U;
  stats_.in_flight = true;
  stats_.in_flight_bytes_sent = 0U;
  return in_flight_;
}

std::shared_ptr<const std::vector<std::byte>>
SessionQueue::begin_serialized_transmission() {
  std::lock_guard lock(mutex_);
  if (!open_ || in_flight_ || in_flight_bytes_ || !pending_bytes_) return {};
  auto result = pending_bytes_->vector_owner();
  if (!result || pending_bytes_->parts().size() != 1U) {
    auto copied = std::make_shared<std::vector<std::byte>>();
    copied->reserve(pending_bytes_->size());
    for (auto part : pending_bytes_->parts()) copied->insert(copied->end(), part.begin(), part.end());
    result = std::move(copied);
  }
  in_flight_bytes_ = std::move(pending_bytes_);
  in_flight_bytes_sent_ = 0U;
  stats_.pending_messages = 0U;
  stats_.in_flight = true;
  stats_.in_flight_bytes_sent = 0U;
  return result;
}

std::shared_ptr<const SerializedBytes> SessionQueue::begin_buffered_transmission() {
  std::lock_guard lock(mutex_);
  if (!open_ || in_flight_ || in_flight_bytes_ || !pending_bytes_) return {};
  in_flight_bytes_ = std::move(pending_bytes_);
  in_flight_bytes_sent_ = 0U;
  stats_.pending_messages = 0U;
  stats_.in_flight = true;
  stats_.in_flight_bytes_sent = 0U;
  return in_flight_bytes_;
}

void SessionQueue::note_bytes_sent(const std::size_t bytes) {
  std::lock_guard lock(mutex_);
  const auto total = in_flight_ ? in_flight_->wire_bytes()
                              : (in_flight_bytes_ ? in_flight_bytes_->size() : 0U);
  if (!open_ || total == 0U || bytes == 0U ||
      bytes > total - in_flight_bytes_sent_) {
    throw std::logic_error("gateway transmission progress is invalid");
  }
  in_flight_bytes_sent_ += bytes;
  stats_.in_flight_bytes_sent = in_flight_bytes_sent_;
}

void SessionQueue::complete_transmission() {
  std::lock_guard lock(mutex_);
  const auto total = in_flight_ ? in_flight_->wire_bytes()
                              : (in_flight_bytes_ ? in_flight_bytes_->size() : 0U);
  if (!open_ || total == 0U || in_flight_bytes_sent_ != total) {
    throw std::logic_error(
        "gateway cannot complete a partial or inactive transmission");
  }
  in_flight_.reset();
  in_flight_bytes_.reset();
  in_flight_bytes_sent_ = 0U;
  ++stats_.transmissions_completed;
  stats_.in_flight = false;
  stats_.in_flight_bytes_sent = 0U;
}

void SessionQueue::close_incomplete() noexcept {
  std::lock_guard lock(mutex_);
  if (!open_) {
    return;
  }
  const auto total = in_flight_ ? in_flight_->wire_bytes()
                              : (in_flight_bytes_ ? in_flight_bytes_->size() : 0U);
  if (total != 0U && in_flight_bytes_sent_ != total) {
    ++stats_.sessions_closed_incomplete;
  }
  open_ = false;
  pending_.reset();
  in_flight_.reset();
  pending_bytes_.reset();
  in_flight_bytes_.reset();
  in_flight_bytes_sent_ = 0U;
  stats_.pending_messages = 0U;
  stats_.in_flight = false;
  stats_.in_flight_bytes_sent = 0U;
  stats_.open = false;
}

SessionQueueStats SessionQueue::stats() const noexcept {
  std::lock_guard lock(mutex_);
  auto result = stats_;
  result.pending_messages = pending_ || pending_bytes_ ? 1U : 0U;
  result.in_flight = static_cast<bool>(in_flight_) || static_cast<bool>(in_flight_bytes_);
  result.in_flight_bytes_sent = in_flight_bytes_sent_;
  result.open = open_;
  return result;
}

void validate(const AdmissionConfiguration& configuration) {
  const auto validate_transport = [](const QualifiedTransport& transport,
                                     const char* name) {
    if (transport.enabled &&
        (!transport.tls_enabled ||
         transport.qualified_payload_bytes_per_second == 0U ||
         transport.maximum_frame_bytes < wire::kHeaderBytes ||
         transport.maximum_frame_bytes >
             wire::kHeaderBytes + wire::kHardMaximumMetadataBytes +
                 wire::kHardMaximumPayloadBytes ||
         transport.maximum_sessions == 0U ||
         transport.maximum_sessions > 1024U)) {
      throw std::invalid_argument(std::string(name) +
                                  " enabled transport has invalid resource bounds");
    }
  };
  validate_transport(configuration.ethernet, "Ethernet");
  validate_transport(configuration.usb_ethernet, "USB Ethernet");
  validate_transport(configuration.wifi, "WiFi");
}

void validate(const SubscriptionRequest& request) {
  if (request.products == 0U ||
      (request.products & ~ipc_v2::kSupportedProducts) != 0U ||
      request.frame_payload_bytes == 0U ||
      request.frame_payload_bytes > wire::kHardMaximumPayloadBytes ||
      request.metadata_bytes == 0U ||
      request.metadata_bytes > wire::kHardMaximumMetadataBytes ||
      !std::isfinite(request.cadence_hz) || request.cadence_hz <= 0.0 ||
      request.cadence_hz > 240.0) {
    throw std::invalid_argument("gateway subscription request is invalid");
  }
}

}  // namespace depthd::gateway
