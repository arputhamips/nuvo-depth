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
  // Omission retains legacy preference behavior; explicit Wi-Fi permission is
  // independent of wired preference. kOff remains the master product switch.
  std::optional<bool> wifi_enabled;
};

[[nodiscard]] bool transport_permitted(TransportPreference preference,
                                       TransportKind transport,
                                       std::optional<bool> wifi_enabled = {});

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
                                      std::optional<TransportPreference> preference = {},
                                      std::optional<bool> wifi_enabled = {});
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

// Immutable serialized bytes may be one mapped message or a small rewritten
// header/metadata followed by views into its owning original payload.
class SerializedBytes final {
 public:
  SerializedBytes(std::vector<std::byte> bytes)
      : SerializedBytes(std::make_shared<const std::vector<std::byte>>(std::move(bytes))) {}
  explicit SerializedBytes(std::shared_ptr<const std::vector<std::byte>> bytes)
      : owner_(bytes), vector_(std::move(bytes)) {
    parts_.emplace_back(*vector_); size_ = vector_->size(); retained_size_ = size_;
  }
  SerializedBytes(std::shared_ptr<const void> owner, std::span<const std::byte> bytes)
      : owner_(std::move(owner)), parts_{bytes}, size_(bytes.size()), retained_size_(size_) {}
  SerializedBytes(std::vector<std::byte> prefix, std::shared_ptr<const SerializedBytes> source,
                  std::vector<std::span<const std::byte>> payload)
      : owner_(source), vector_(std::make_shared<const std::vector<std::byte>>(std::move(prefix))) {
    parts_.emplace_back(*vector_);
    parts_.insert(parts_.end(), payload.begin(), payload.end());
    for (auto part : parts_) size_ += part.size();
    retained_size_ = source->retained_size() + vector_->size();
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t retained_size() const noexcept { return retained_size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
  [[nodiscard]] const std::byte* data() const noexcept { return parts_.front().data(); }
  [[nodiscard]] std::span<const std::byte> prefix() const noexcept { return parts_.front(); }
  [[nodiscard]] const auto& parts() const noexcept { return parts_; }
  [[nodiscard]] const auto& vector_owner() const noexcept { return vector_; }
  [[nodiscard]] std::vector<unsigned char> copy() const {
    std::vector<unsigned char> result; result.reserve(size_);
    for (auto part : parts_) result.insert(result.end(),
        reinterpret_cast<const unsigned char*>(part.data()),
        reinterpret_cast<const unsigned char*>(part.data() + part.size()));
    return result;
  }
 private:
  std::shared_ptr<const void> owner_;
  std::shared_ptr<const std::vector<std::byte>> vector_;
  std::vector<std::span<const std::byte>> parts_;
  std::size_t size_{}, retained_size_{};
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
  [[nodiscard]] OfferResult offer_buffered(std::shared_ptr<const SerializedBytes> bytes);
  [[nodiscard]] std::shared_ptr<const SerializedBytes> begin_buffered_transmission();
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
  std::shared_ptr<const SerializedBytes> pending_bytes_;
  std::shared_ptr<const SerializedBytes> in_flight_bytes_;
  std::size_t in_flight_bytes_sent_{};
  SessionQueueStats stats_{};
  bool open_{true};
};

void validate(const AdmissionConfiguration& configuration);
void validate(const SubscriptionRequest& request);

}  // namespace depthd::gateway
