#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "depthd/ipc_v2.hpp"
#include "depthd/wire_protocol.hpp"

namespace depthd::gateway {

enum class TransportKind : std::uint8_t {
  kEthernet = 1,
  kUsbEthernet = 2,
  kWifi = 3,
};

enum class TransportPreference : std::uint8_t {
  kEthernetPreferred = 1,
  kUsbPreferred = 2,
  kBoth = 3,
  kOff = 4,
  kAuto = 5,
  kWifiOnly = 6,
};

struct QualifiedTransport {
  bool enabled{};
  bool tls_enabled{true};
  std::uint64_t qualified_payload_bytes_per_second{};
  std::uint64_t maximum_frame_bytes{};
  std::size_t maximum_sessions{};
};

struct AdmissionConfiguration {
  QualifiedTransport ethernet{};
  QualifiedTransport usb_ethernet{};
  QualifiedTransport wifi{};
  TransportPreference preference{TransportPreference::kEthernetPreferred};
};

struct SubscriptionRequest {
  ipc_v2::ProductMask products{};
  std::uint64_t frame_payload_bytes{};
  std::uint32_t metadata_bytes{};
  double cadence_hz{};
  bool allow_format_change{};
  bool allow_cadence_reduction{};
};

class AdmissionLease final {
 public:
  AdmissionLease() = default;
  ~AdmissionLease() noexcept;
  AdmissionLease(const AdmissionLease&) = delete;
  AdmissionLease& operator=(const AdmissionLease&) = delete;
  AdmissionLease(AdmissionLease&&) noexcept;
  AdmissionLease& operator=(AdmissionLease&&) noexcept;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] TransportKind transport() const;
  [[nodiscard]] std::uint64_t reserved_bytes_per_second() const;
  void reset() noexcept;

 private:
  struct Impl;
  explicit AdmissionLease(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class AdmissionController;
};

struct AdmissionResult {
  std::optional<AdmissionLease> lease;
  std::string reason;
  std::vector<TransportKind> compatible_alternatives;
};

struct AdmissionStats {
  std::uint64_t ethernet_reserved_bytes_per_second{};
  std::uint64_t usb_reserved_bytes_per_second{};
  std::uint64_t wifi_reserved_bytes_per_second{};
  std::size_t ethernet_sessions{};
  std::size_t usb_sessions{};
  std::size_t wifi_sessions{};
  std::uint64_t admitted{};
  std::uint64_t rejected{};
};

struct AdmissionState;

// One controller reserves requested framed rates for the actual Ethernet,
// USB-Ethernet, or WiFi connection. Bounds may represent measured qualification
// or explicit best-effort resource limits; the executable labels that contract.
// Reservation never silently changes units, products, format, or cadence.
class AdmissionController final {
 public:
  explicit AdmissionController(AdmissionConfiguration configuration);
  [[nodiscard]] AdmissionResult admit(const SubscriptionRequest& request);
  // A connection cannot migrate to another transport after accept. This
  // overload reserves only the capacity of its actual receiving interface.
  [[nodiscard]] AdmissionResult admit(const SubscriptionRequest& request,
                                      TransportKind connected_transport,
                                      std::optional<TransportPreference> preference = {});
  [[nodiscard]] AdmissionStats stats() const noexcept;

 private:
  std::shared_ptr<AdmissionState> state_;
};

enum class OfferResult : std::uint8_t {
  kAccepted = 0,
  kReplacedPendingComplete = 1,
  kClosed = 2,
  kOversize = 3,
};

struct SessionQueueStats {
  std::uint64_t offered{};
  std::uint64_t pending_replaced{};
  std::uint64_t transmissions_completed{};
  std::uint64_t sessions_closed_incomplete{};
  std::size_t pending_messages{};
  bool in_flight{};
  std::size_t in_flight_bytes_sent{};
  bool open{};
};

// A network client owns at most one unsent complete frame plus one in-flight
// message. Once any byte is sent, the message must finish or the session must
// close; framing is never resumed after abandoning a partial message.
class SessionQueue final {
 public:
  explicit SessionQueue(std::size_t maximum_wire_bytes);

  [[nodiscard]] OfferResult offer(std::shared_ptr<const wire::Message> message);
  [[nodiscard]] OfferResult offer_serialized(
      std::shared_ptr<const std::vector<std::byte>> bytes);
  [[nodiscard]] std::shared_ptr<const wire::Message> begin_transmission();
  [[nodiscard]] std::shared_ptr<const std::vector<std::byte>>
      begin_serialized_transmission();
  void note_bytes_sent(std::size_t bytes);
  void complete_transmission();
  void close_incomplete() noexcept;
  [[nodiscard]] SessionQueueStats stats() const noexcept;

 private:
  const std::size_t maximum_wire_bytes_;
  mutable std::mutex mutex_;
  std::shared_ptr<const wire::Message> pending_;
  std::shared_ptr<const wire::Message> in_flight_;
  std::shared_ptr<const std::vector<std::byte>> pending_bytes_;
  std::shared_ptr<const std::vector<std::byte>> in_flight_bytes_;
  std::size_t in_flight_bytes_sent_{};
  SessionQueueStats stats_{};
  bool open_{true};
};

void validate(const AdmissionConfiguration& configuration);
void validate(const SubscriptionRequest& request);

}  // namespace depthd::gateway
