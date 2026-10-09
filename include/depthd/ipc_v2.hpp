#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "depthd/runtime.hpp"
#include "depthd/output_selection.hpp"

namespace depthd::ipc_v2 {

inline constexpr std::uint32_t kSchemaVersion = 2U;
enum class DeliveryTrust : std::uint8_t {
  kUntrustedCopy = 0,
  kTrustedZeroCopy = 1,
};

struct Subscription {
  ProductMask products{};
  DeliveryTrust delivery{DeliveryTrust::kUntrustedCopy};
  std::size_t maximum_outstanding_leases{1U};
};

struct PoolConfiguration {
  std::size_t slot_count{6U};
  std::size_t maximum_subscribers{32U};
  std::size_t maximum_trusted_zero_copy_subscribers{2U};
  std::array<std::size_t, kProductCount> product_capacity_bytes{};
  std::size_t maximum_total_bytes{512U * 1024U * 1024U};
};

struct FrameDescriptor {
  std::uint32_t schema_version{kSchemaVersion};
  FrameSetIdentity frame{};
  ProductSetMetadata products{};
  ProductMask available_products{kAllProducts};
  std::array<std::size_t, kProductCount> product_bytes{};
  std::uint64_t publication_sequence{};
};

struct BrokerStats {
  std::uint64_t published{};
  std::uint64_t publish_rejected_nonmonotonic{};
  std::uint64_t publish_rejected_layout{};
  std::uint64_t publish_rejected_oversize{};
  std::uint64_t publish_dropped_all_slots_pinned{};
  std::uint64_t zero_copy_leases_granted{};
  std::uint64_t copy_deliveries{};
  std::uint64_t subscriber_limit_rejections{};
  std::size_t subscribers{};
  std::size_t pinned_slots{};
};

using SubscriptionId = std::uint64_t;
struct BrokerState;

enum class PublishResult : std::uint8_t {
  kPublished = 0,
  kAllSlotsPinned = 1,
  kOversize = 2,
  kNonmonotonic = 3,
  kLayoutMismatch = 4,
};

class ReadLease final {
 public:
  ReadLease() = default;
  ~ReadLease() noexcept;
  ReadLease(const ReadLease&) = delete;
  ReadLease& operator=(const ReadLease&) = delete;
  ReadLease(ReadLease&&) noexcept;
  ReadLease& operator=(ReadLease&&) noexcept;

  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] const FrameDescriptor& descriptor() const;
  [[nodiscard]] std::span<const std::byte> product(ProductKind kind) const;
  void reset() noexcept;

 private:
  struct Impl;
  explicit ReadLease(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class Broker;
};

struct CopiedFrame {
  FrameDescriptor descriptor{};
  ProductMask selected_products{};
  std::array<std::vector<std::byte>, kProductCount> storage{};

  [[nodiscard]] std::span<const std::byte> product(ProductKind kind) const;
};

// The broker is the platform-independent ownership core for IPC v2. Each slot
// has physically separate product storage. A genuine read lease pins its slot;
// no timeout or disconnect can make that slot writable before release.
class Broker final {
 public:
  Broker(PoolConfiguration configuration,
         std::array<std::uint8_t, 16> run_uuid);
  ~Broker();

  Broker(const Broker&) = delete;
  Broker& operator=(const Broker&) = delete;

  [[nodiscard]] std::optional<SubscriptionId> subscribe(
      const Subscription& subscription);
  void disconnect(SubscriptionId subscriber) noexcept;

  [[nodiscard]] PublishResult publish(const FrameSetStorage& frame);
  [[nodiscard]] std::optional<ReadLease> acquire_latest_zero_copy(
      SubscriptionId subscriber);
  [[nodiscard]] std::optional<CopiedFrame> copy_latest(
      SubscriptionId subscriber);
  [[nodiscard]] BrokerStats stats() const noexcept;

 private:
  std::shared_ptr<BrokerState> state_;
};

[[nodiscard]] PoolConfiguration make_pool_configuration(
    const ProductSetMetadata& products, std::size_t slot_count = 6U);
void validate(const Subscription& subscription);
void validate(const PoolConfiguration& configuration);
void validate(const FrameDescriptor& descriptor);

}  // namespace depthd::ipc_v2
