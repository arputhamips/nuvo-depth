#include "depthd/ipc_v2.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace depthd::ipc_v2 {
namespace {

[[nodiscard]] constexpr std::size_t index_of(const ProductKind kind) {
  return static_cast<std::size_t>(kind);
}

[[nodiscard]] bool uuid_present(
    const std::array<std::uint8_t, 16>& value) noexcept {
  return std::any_of(value.begin(), value.end(),
                     [](const std::uint8_t byte) { return byte != 0U; });
}

[[nodiscard]] std::size_t checked_multiply(const std::size_t left,
                                           const std::size_t right) {
  if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::overflow_error("IPC v2 pool byte size overflows");
  }
  return left * right;
}

[[nodiscard]] std::size_t image_channels(const ImageProductFormat format) {
  return format == ImageProductFormat::kGray8 ? 1U : 3U;
}

[[nodiscard]] std::size_t grid_bytes(const PixelGrid& grid,
                                     const std::size_t element_bytes) {
  validate(grid);
  return checked_multiply(
      checked_multiply(static_cast<std::size_t>(grid.width), grid.height),
      element_bytes);
}

[[nodiscard]] std::array<std::size_t, kProductCount> expected_product_bytes(
    const ProductSetMetadata& products) {
  validate(products);
  std::array<std::size_t, kProductCount> result{};
  result[index_of(ProductKind::kLeftRectified)] =
      grid_bytes(products.left_rectified.grid,
                 image_channels(products.left_rectified.format));
  result[index_of(ProductKind::kRightRectified)] =
      grid_bytes(products.right_rectified.grid,
                 image_channels(products.right_rectified.format));
  result[index_of(ProductKind::kDisparity)] =
      grid_bytes(products.disparity.grid, sizeof(float));
  result[index_of(ProductKind::kDepth)] =
      grid_bytes(products.depth.grid, sizeof(float));
  result[index_of(ProductKind::kConfidence)] =
      grid_bytes(products.disparity.grid, sizeof(std::uint8_t));
  result[index_of(ProductKind::kValidity)] =
      grid_bytes(products.disparity.grid, sizeof(std::uint8_t));
  return result;
}

struct SourceProduct {
  const std::byte* data{};
  std::size_t bytes{};
};

[[nodiscard]] std::array<SourceProduct, kProductCount> source_products(
    const FrameSetStorage& frame) {
  return {{{reinterpret_cast<const std::byte*>(frame.left_rectified.data()),
            frame.left_rectified.size()},
           {reinterpret_cast<const std::byte*>(frame.right_rectified.data()),
            frame.right_rectified.size()},
           {reinterpret_cast<const std::byte*>(frame.disparity.data()),
            frame.disparity.size() * sizeof(float)},
           {reinterpret_cast<const std::byte*>(frame.depth_m.data()),
            frame.depth_m.size() * sizeof(float)},
           {reinterpret_cast<const std::byte*>(frame.confidence.data()),
            frame.confidence.size()},
           {reinterpret_cast<const std::byte*>(frame.validity.data()),
            frame.validity.size()}}};
}

void require_product(const ProductMask mask, const ProductKind kind) {
  if ((mask & product_bit(kind)) == 0U) {
    throw std::invalid_argument("product was not selected for this delivery");
  }
}

}  // namespace

struct BrokerState {
  struct Slot {
    std::array<std::vector<std::byte>, kProductCount> products;
    FrameDescriptor descriptor{};
    std::size_t readers{};
    bool writing{};
    bool published{};
  };

  struct Subscriber {
    Subscription subscription{};
    std::uint64_t last_publication_sequence{};
    std::size_t outstanding{};
  };

  PoolConfiguration configuration{};
  std::array<std::uint8_t, 16> run_uuid{};
  mutable std::mutex mutex;
  std::vector<Slot> slots;
  std::map<SubscriptionId, Subscriber> subscribers;
  SubscriptionId next_subscriber{1U};
  std::uint64_t next_publication_sequence{1U};
  std::uint64_t last_capture_epoch{};
  std::uint64_t last_capture_sequence{};
  BrokerStats stats{};
};

struct ReadLease::Impl {
  std::shared_ptr<BrokerState> state;
  SubscriptionId subscriber{};
  std::size_t slot_index{};
  std::uint64_t publication_sequence{};
  ProductMask products{};
};

ReadLease::ReadLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ReadLease::~ReadLease() noexcept { reset(); }
ReadLease::ReadLease(ReadLease&&) noexcept = default;
ReadLease& ReadLease::operator=(ReadLease&& other) noexcept {
  if (this != &other) {
    reset();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

ReadLease::operator bool() const noexcept { return impl_ != nullptr; }

const FrameDescriptor& ReadLease::descriptor() const {
  if (!impl_) {
    throw std::logic_error("IPC v2 read lease is inactive");
  }
  const auto& slot = impl_->state->slots.at(impl_->slot_index);
  if (!slot.published ||
      slot.descriptor.publication_sequence != impl_->publication_sequence) {
    throw std::logic_error("IPC v2 read lease generation changed");
  }
  return slot.descriptor;
}

std::span<const std::byte> ReadLease::product(const ProductKind kind) const {
  require_product(impl_ ? impl_->products : 0U, kind);
  const auto& slot = impl_->state->slots.at(impl_->slot_index);
  if (!slot.published ||
      slot.descriptor.publication_sequence != impl_->publication_sequence) {
    throw std::logic_error("IPC v2 read lease generation changed");
  }
  const auto product_index = index_of(kind);
  return {slot.products[product_index].data(),
          slot.descriptor.product_bytes[product_index]};
}

void ReadLease::reset() noexcept {
  auto impl = std::move(impl_);
  if (!impl) {
    return;
  }
  std::lock_guard lock(impl->state->mutex);
  if (impl->slot_index < impl->state->slots.size()) {
    auto& slot = impl->state->slots[impl->slot_index];
    if (slot.published &&
        slot.descriptor.publication_sequence == impl->publication_sequence &&
        slot.readers != 0U) {
      --slot.readers;
    }
  }
  const auto subscriber = impl->state->subscribers.find(impl->subscriber);
  if (subscriber != impl->state->subscribers.end() &&
      subscriber->second.outstanding != 0U) {
    --subscriber->second.outstanding;
  }
}

std::span<const std::byte> CopiedFrame::product(const ProductKind kind) const {
  require_product(selected_products, kind);
  const auto& bytes = storage[index_of(kind)];
  return {bytes.data(), bytes.size()};
}

Broker::Broker(PoolConfiguration configuration,
               std::array<std::uint8_t, 16> run_uuid)
    : state_(std::make_shared<BrokerState>()) {
  validate(configuration);
  if (!uuid_present(run_uuid)) {
    throw std::invalid_argument("IPC v2 broker requires a nonzero run UUID");
  }
  state_->configuration = configuration;
  state_->run_uuid = run_uuid;
  state_->slots.resize(configuration.slot_count);
  for (auto& slot : state_->slots) {
    for (std::size_t product = 0; product < kProductCount; ++product) {
      slot.products[product].resize(
          configuration.product_capacity_bytes[product]);
    }
  }
}

Broker::~Broker() = default;

std::optional<SubscriptionId> Broker::subscribe(
    const Subscription& subscription) {
  validate(subscription);
  std::lock_guard lock(state_->mutex);
  const auto trusted_count = static_cast<std::size_t>(std::count_if(
      state_->subscribers.begin(), state_->subscribers.end(),
      [](const auto& item) {
        return item.second.subscription.delivery ==
               DeliveryTrust::kTrustedZeroCopy;
      }));
  if (state_->subscribers.size() >= state_->configuration.maximum_subscribers ||
      (subscription.delivery == DeliveryTrust::kTrustedZeroCopy &&
       trusted_count >=
           state_->configuration.maximum_trusted_zero_copy_subscribers)) {
    ++state_->stats.subscriber_limit_rejections;
    return std::nullopt;
  }
  const auto id = state_->next_subscriber++;
  state_->subscribers.emplace(id, BrokerState::Subscriber{subscription});
  state_->stats.subscribers = state_->subscribers.size();
  return id;
}

void Broker::disconnect(const SubscriptionId subscriber) noexcept {
  std::lock_guard lock(state_->mutex);
  state_->subscribers.erase(subscriber);
  state_->stats.subscribers = state_->subscribers.size();
}

PublishResult Broker::publish(const FrameSetStorage& frame) {
  validate(frame.metadata);
  const auto sources = source_products(frame);
  const auto expected = expected_product_bytes(frame.metadata);
  for (std::size_t index = 0; index < kProductCount; ++index) {
    if (expected[index] > state_->configuration.product_capacity_bytes[index]) {
      std::lock_guard lock(state_->mutex);
      ++state_->stats.publish_rejected_oversize;
      return PublishResult::kOversize;
    }
    if (sources[index].data == nullptr || sources[index].bytes != expected[index]) {
      std::lock_guard lock(state_->mutex);
      ++state_->stats.publish_rejected_layout;
      return PublishResult::kLayoutMismatch;
    }
  }
  if (frame.metadata.frame.run_uuid != state_->run_uuid) {
    std::lock_guard lock(state_->mutex);
    ++state_->stats.publish_rejected_nonmonotonic;
    return PublishResult::kNonmonotonic;
  }

  std::size_t selected = state_->slots.size();
  {
    std::lock_guard lock(state_->mutex);
    const auto epoch = frame.metadata.frame.capture_epoch;
    const auto sequence = frame.metadata.frame.capture_sequence;
    if (epoch < state_->last_capture_epoch ||
        (epoch == state_->last_capture_epoch &&
         sequence <= state_->last_capture_sequence)) {
      ++state_->stats.publish_rejected_nonmonotonic;
      return PublishResult::kNonmonotonic;
    }
    std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < state_->slots.size(); ++index) {
      const auto& slot = state_->slots[index];
      if (!slot.writing && slot.readers == 0U &&
          (!slot.published ||
           slot.descriptor.publication_sequence < oldest)) {
        selected = index;
        oldest = slot.published ? slot.descriptor.publication_sequence : 0U;
        if (!slot.published) {
          break;
        }
      }
    }
    if (selected == state_->slots.size()) {
      ++state_->stats.publish_dropped_all_slots_pinned;
      return PublishResult::kAllSlotsPinned;
    }
    auto& slot = state_->slots[selected];
    slot.writing = true;
    slot.published = false;
  }

  auto& slot = state_->slots[selected];
  try {
    for (std::size_t index = 0; index < kProductCount; ++index) {
      std::memcpy(slot.products[index].data(), sources[index].data,
                  sources[index].bytes);
    }
  } catch (...) {
    std::lock_guard lock(state_->mutex);
    slot.writing = false;
    throw;
  }

  std::lock_guard lock(state_->mutex);
  const auto epoch = frame.metadata.frame.capture_epoch;
  const auto sequence = frame.metadata.frame.capture_sequence;
  if (epoch < state_->last_capture_epoch ||
      (epoch == state_->last_capture_epoch &&
       sequence <= state_->last_capture_sequence)) {
    slot.writing = false;
    ++state_->stats.publish_rejected_nonmonotonic;
    return PublishResult::kNonmonotonic;
  }
  slot.descriptor = {};
  slot.descriptor.frame = frame.metadata.frame;
  slot.descriptor.products = frame.metadata;
  slot.descriptor.available_products = kAllProducts;
  for (std::size_t index = 0; index < kProductCount; ++index) {
    slot.descriptor.product_bytes[index] = sources[index].bytes;
  }
  slot.descriptor.publication_sequence = state_->next_publication_sequence++;
  validate(slot.descriptor);
  slot.writing = false;
  slot.published = true;
  state_->last_capture_epoch = epoch;
  state_->last_capture_sequence = sequence;
  ++state_->stats.published;
  return PublishResult::kPublished;
}

std::optional<ReadLease> Broker::acquire_latest_zero_copy(
    const SubscriptionId subscriber_id) {
  std::lock_guard lock(state_->mutex);
  const auto subscriber = state_->subscribers.find(subscriber_id);
  if (subscriber == state_->subscribers.end() ||
      subscriber->second.subscription.delivery !=
          DeliveryTrust::kTrustedZeroCopy ||
      subscriber->second.outstanding >=
          subscriber->second.subscription.maximum_outstanding_leases) {
    return std::nullopt;
  }
  std::size_t selected = state_->slots.size();
  std::uint64_t latest = subscriber->second.last_publication_sequence;
  for (std::size_t index = 0; index < state_->slots.size(); ++index) {
    const auto& slot = state_->slots[index];
    if (slot.published && !slot.writing &&
        slot.descriptor.publication_sequence > latest) {
      selected = index;
      latest = slot.descriptor.publication_sequence;
    }
  }
  if (selected == state_->slots.size()) {
    return std::nullopt;
  }
  auto& slot = state_->slots[selected];
  ++slot.readers;
  ++subscriber->second.outstanding;
  subscriber->second.last_publication_sequence = latest;
  ++state_->stats.zero_copy_leases_granted;
  auto impl = std::make_unique<ReadLease::Impl>();
  impl->state = state_;
  impl->subscriber = subscriber_id;
  impl->slot_index = selected;
  impl->publication_sequence = latest;
  impl->products = subscriber->second.subscription.products;
  return ReadLease(std::move(impl));
}

std::optional<CopiedFrame> Broker::copy_latest(
    const SubscriptionId subscriber_id) {
  std::lock_guard lock(state_->mutex);
  const auto subscriber = state_->subscribers.find(subscriber_id);
  if (subscriber == state_->subscribers.end() ||
      subscriber->second.subscription.delivery !=
          DeliveryTrust::kUntrustedCopy) {
    return std::nullopt;
  }
  const BrokerState::Slot* selected = nullptr;
  for (const auto& slot : state_->slots) {
    if (slot.published && !slot.writing &&
        slot.descriptor.publication_sequence >
            subscriber->second.last_publication_sequence &&
        (!selected || slot.descriptor.publication_sequence >
                          selected->descriptor.publication_sequence)) {
      selected = &slot;
    }
  }
  if (!selected) {
    return std::nullopt;
  }
  CopiedFrame result;
  result.descriptor = selected->descriptor;
  result.selected_products = subscriber->second.subscription.products;
  for (std::size_t index = 0; index < kProductCount; ++index) {
    if ((result.selected_products & (1U << index)) != 0U) {
      const auto bytes = selected->descriptor.product_bytes[index];
      result.storage[index].assign(selected->products[index].begin(),
                                   selected->products[index].begin() +
                                       static_cast<std::ptrdiff_t>(bytes));
    }
  }
  subscriber->second.last_publication_sequence =
      selected->descriptor.publication_sequence;
  ++state_->stats.copy_deliveries;
  return result;
}

BrokerStats Broker::stats() const noexcept {
  std::lock_guard lock(state_->mutex);
  auto result = state_->stats;
  result.subscribers = state_->subscribers.size();
  result.pinned_slots = static_cast<std::size_t>(std::count_if(
      state_->slots.begin(), state_->slots.end(),
      [](const BrokerState::Slot& slot) { return slot.readers != 0U; }));
  return result;
}

PoolConfiguration make_pool_configuration(const ProductSetMetadata& products,
                                           const std::size_t slot_count) {
  PoolConfiguration result;
  result.slot_count = slot_count;
  result.product_capacity_bytes = expected_product_bytes(products);
  validate(result);
  return result;
}

void validate(const Subscription& subscription) {
  if (subscription.products == 0U ||
      (subscription.products & ~kAllProducts) != 0U) {
    throw std::invalid_argument("IPC v2 subscription product mask is invalid");
  }
  if (subscription.maximum_outstanding_leases == 0U ||
      subscription.maximum_outstanding_leases > 6U) {
    throw std::invalid_argument(
        "IPC v2 subscription outstanding lease limit must be 1..6");
  }
  if (subscription.delivery == DeliveryTrust::kUntrustedCopy &&
      subscription.maximum_outstanding_leases != 1U) {
    throw std::invalid_argument(
        "copy subscriptions cannot request zero-copy lease allowance");
  }
}

void validate(const PoolConfiguration& configuration) {
  if (configuration.slot_count < 2U || configuration.slot_count > 64U ||
      configuration.maximum_subscribers == 0U ||
      configuration.maximum_subscribers > 1024U ||
      configuration.maximum_trusted_zero_copy_subscribers >
          configuration.maximum_subscribers ||
      configuration.maximum_total_bytes == 0U) {
    throw std::invalid_argument("IPC v2 pool limits are invalid");
  }
  std::size_t bytes_per_slot{};
  for (const auto capacity : configuration.product_capacity_bytes) {
    if (capacity == 0U ||
        capacity > configuration.maximum_total_bytes - bytes_per_slot) {
      throw std::invalid_argument("IPC v2 product capacity is invalid");
    }
    bytes_per_slot += capacity;
  }
  if (bytes_per_slot > configuration.maximum_total_bytes /
                           configuration.slot_count) {
    throw std::invalid_argument("IPC v2 pool exceeds its memory budget");
  }
}

void validate(const FrameDescriptor& descriptor) {
  if (descriptor.schema_version != kSchemaVersion ||
      descriptor.publication_sequence == 0U ||
      descriptor.available_products == 0U ||
      (descriptor.available_products & ~kAllProducts) != 0U ||
      descriptor.frame.capture_epoch == 0U ||
      descriptor.frame.capture_sequence == 0U ||
      descriptor.frame.run_uuid != descriptor.products.frame.run_uuid ||
      descriptor.frame.capture_epoch !=
          descriptor.products.frame.capture_epoch ||
      descriptor.frame.capture_sequence !=
          descriptor.products.frame.capture_sequence) {
    throw std::invalid_argument("IPC v2 frame descriptor is incoherent");
  }
  validate(descriptor.products);
  for (std::size_t index = 0; index < kProductCount; ++index) {
    if ((descriptor.available_products & (1U << index)) != 0U &&
        descriptor.product_bytes[index] == 0U) {
      throw std::invalid_argument("IPC v2 available product has no bytes");
    }
  }
}

}  // namespace depthd::ipc_v2
