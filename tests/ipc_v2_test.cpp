#include "depthd/ipc_v2.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

depthd::FrameSetStorage frame(const std::uint64_t sequence,
                              const std::array<std::uint8_t, 16>& run_uuid,
                              const float depth_value) {
  depthd::FrameSetStorage result;
  const auto image_grid = depthd::make_reference_grid(4U, 4U);
  const auto matching_grid =
      depthd::make_resampled_grid(image_grid, 2U, 2U);
  result.metadata.frame.device_uuid[0] = 1U;
  result.metadata.frame.run_uuid = run_uuid;
  result.metadata.frame.capture_epoch = 1U;
  result.metadata.frame.capture_sequence = sequence;
  result.metadata.frame.configuration_epoch = 1U;
  result.metadata.frame.capture_time_ns =
      1000 + static_cast<std::int64_t>(sequence);
  result.metadata.frame.capture_clock = depthd::CaptureClock::kBoottime;
  result.metadata.calibration = depthd::calibration_identity_from_sha256(
      "3333333333333333333333333333333333333333333333333333333333333333");
  result.metadata.calibration_authority =
      depthd::CalibrationAuthority::kFactoryValidated;
  result.metadata.left_rectified = {
      image_grid, {0U, 0U, 4U, 4U}, depthd::CameraRole::kLogicalLeft,
      depthd::ImageProductFormat::kGray8};
  result.metadata.right_rectified = {
      image_grid, {0U, 0U, 4U, 4U}, depthd::CameraRole::kLogicalRight,
      depthd::ImageProductFormat::kGray8};
  result.metadata.disparity.grid = matching_grid;
  result.metadata.disparity.valid_roi = {0U, 0U, 2U, 2U};
  result.metadata.disparity.accepted_min_disparity_px = 0.0F;
  result.metadata.disparity.accepted_max_disparity_px = 16.0F;
  result.metadata.disparity.algorithmic_quantization_px = 1.0F / 16.0F;
  result.metadata.disparity.confidence_model =
      depthd::ConfidenceModel::kBinarySgbmSupportV1;
  result.metadata.depth = {matching_grid, 0.01F, 100.0F};
  result.left_rectified.assign(16U, static_cast<std::uint8_t>(sequence));
  result.right_rectified.assign(16U,
                                static_cast<std::uint8_t>(sequence + 10U));
  result.disparity.assign(4U, 5.0F);
  result.depth_m.assign(4U, depth_value);
  result.confidence.assign(4U, 255U);
  result.validity.assign(
      4U, static_cast<std::uint8_t>(depthd::ValidityCode::kValid));
  (void)result.view();
  return result;
}

float first_float(const std::span<const std::byte> bytes) {
  require(bytes.size() >= sizeof(float), "float product is truncated");
  float result{};
  std::memcpy(&result, bytes.data(), sizeof(result));
  return result;
}

void test_selective_copy_and_generation() {
  std::array<std::uint8_t, 16> run_uuid{};
  run_uuid[0] = 9U;
  auto first = frame(1U, run_uuid, 1.25F);
  auto config = depthd::ipc_v2::make_pool_configuration(first.metadata, 2U);
  depthd::ipc_v2::Broker broker(config, run_uuid);
  const auto subscriber = broker.subscribe(
      {depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kDepth),
       depthd::ipc_v2::DeliveryTrust::kUntrustedCopy, 1U});
  require(subscriber.has_value(), "copy subscriber was not admitted");
  require(broker.publish(first) == depthd::ipc_v2::PublishResult::kPublished,
          "first frame was not published");
  auto copied = broker.copy_latest(*subscriber);
  require(copied.has_value() &&
              copied->descriptor.frame.capture_sequence == 1U &&
              std::abs(first_float(copied->product(
                           depthd::ipc_v2::ProductKind::kDepth)) -
                       1.25F) < 1.0e-6F,
          "selective copy did not preserve descriptor and depth bytes");
  bool hid_image = false;
  try {
    (void)copied->product(depthd::ipc_v2::ProductKind::kLeftRectified);
  } catch (const std::invalid_argument&) {
    hid_image = true;
  }
  require(hid_image, "depth-only subscriber could access image storage");
  require(!broker.copy_latest(*subscriber).has_value(),
          "subscriber received the same generation twice");
}

void test_pinned_slots_are_never_recycled() {
  std::array<std::uint8_t, 16> run_uuid{};
  run_uuid[0] = 7U;
  auto seed = frame(1U, run_uuid, 1.0F);
  auto config = depthd::ipc_v2::make_pool_configuration(seed.metadata, 2U);
  config.maximum_trusted_zero_copy_subscribers = 1U;
  depthd::ipc_v2::Broker broker(config, run_uuid);
  const auto subscriber = broker.subscribe(
      {depthd::ipc_v2::kAllProducts,
       depthd::ipc_v2::DeliveryTrust::kTrustedZeroCopy, 2U});
  require(subscriber.has_value(), "trusted subscriber was not admitted");

  require(broker.publish(seed) == depthd::ipc_v2::PublishResult::kPublished,
          "seed publish failed");
  auto lease_one = broker.acquire_latest_zero_copy(*subscriber);
  require(lease_one.has_value(), "first zero-copy lease was not granted");
  const auto held_value = first_float(
      lease_one->product(depthd::ipc_v2::ProductKind::kDepth));

  auto second = frame(2U, run_uuid, 2.0F);
  require(broker.publish(second) == depthd::ipc_v2::PublishResult::kPublished,
          "second publish failed with one free slot");
  auto lease_two = broker.acquire_latest_zero_copy(*subscriber);
  require(lease_two.has_value(), "second zero-copy lease was not granted");
  auto third = frame(3U, run_uuid, 3.0F);
  require(broker.publish(third) ==
              depthd::ipc_v2::PublishResult::kAllSlotsPinned,
          "publisher recycled a live-read slot");
  require(first_float(
              lease_one->product(depthd::ipc_v2::ProductKind::kDepth)) ==
              held_value,
          "failed publication changed a pinned product");

  broker.disconnect(*subscriber);
  require(broker.publish(third) ==
              depthd::ipc_v2::PublishResult::kAllSlotsPinned,
          "disconnect incorrectly revoked genuine mappings");
  lease_one->reset();
  require(broker.publish(third) == depthd::ipc_v2::PublishResult::kPublished,
          "released slot was not returned to producer capacity");
  lease_two->reset();
  const auto stats = broker.stats();
  require(stats.publish_dropped_all_slots_pinned == 2U &&
              stats.pinned_slots == 0U && stats.subscribers == 0U,
          "IPC v2 pinned-slot accounting is incorrect");
}

void test_limits_and_identity_rejections() {
  std::array<std::uint8_t, 16> run_uuid{};
  run_uuid[0] = 5U;
  auto seed = frame(1U, run_uuid, 1.0F);
  auto config = depthd::ipc_v2::make_pool_configuration(seed.metadata, 2U);
  config.maximum_subscribers = 1U;
  config.maximum_trusted_zero_copy_subscribers = 0U;
  depthd::ipc_v2::Broker broker(config, run_uuid);
  require(!broker.subscribe(
               {depthd::ipc_v2::kAllProducts,
                depthd::ipc_v2::DeliveryTrust::kTrustedZeroCopy, 1U})
               .has_value(),
          "trusted subscriber bypassed its admission limit");
  require(broker.publish(seed) == depthd::ipc_v2::PublishResult::kPublished,
          "valid seed publish failed");
  require(broker.publish(seed) ==
              depthd::ipc_v2::PublishResult::kNonmonotonic,
          "duplicate capture identity was accepted");

  auto wrong_run = run_uuid;
  wrong_run[0] = 6U;
  require(broker.publish(frame(2U, wrong_run, 2.0F)) ==
              depthd::ipc_v2::PublishResult::kNonmonotonic,
          "old or foreign run generation was accepted");
  auto oversized = frame(2U, run_uuid, 2.0F);
  oversized.left_rectified.push_back(0U);
  require(broker.publish(oversized) ==
              depthd::ipc_v2::PublishResult::kLayoutMismatch,
          "product storage larger than its descriptor was accepted");
  auto truncated = frame(2U, run_uuid, 2.0F);
  truncated.depth_m.pop_back();
  require(broker.publish(truncated) ==
              depthd::ipc_v2::PublishResult::kLayoutMismatch,
          "truncated product disagreed with metadata but was accepted");
}

void test_extended_selective_publication_hides_unavailable_products() {
  using namespace depthd::ipc_v2;
  std::array<std::uint8_t, 16> run{}; run[0] = 15U;
  auto input = frame(1U, run, 1.0F);
  auto configuration = make_pool_configuration(input.metadata, 2U);
  Broker broker(configuration, run);
  const auto rgb = broker.subscribe({product_bit(ProductKind::kLeftRgb), DeliveryTrust::kUntrustedCopy, 1U});
  const auto depth = broker.subscribe({product_bit(ProductKind::kDepth), DeliveryTrust::kUntrustedCopy, 1U});
  require(rgb && depth, "extended subscriptions were not admitted");
  input.available_products = product_bit(ProductKind::kLeftRgb) | product_bit(ProductKind::kRectifiedStereoRgb);
  input.left_rectified.clear(); input.right_rectified.clear(); input.disparity.clear();
  input.depth_m.clear(); input.confidence.clear(); input.validity.clear();
  input.left_rgb.assign(48U, 71U); input.rectified_stereo_rgb.assign(96U, 91U);
  require(broker.publish(input) == PublishResult::kPublished, "extended selective frame was rejected");
  const auto copy = broker.copy_latest(*rgb);
  require(copy && copy->descriptor.available_products == input.available_products &&
          copy->product(ProductKind::kLeftRgb).size() == 48U &&
          copy->product(ProductKind::kLeftRgb).front() == std::byte{71},
          "extended product copy lost mask, layout or pixels");
  require(!broker.copy_latest(*depth), "subscriber received a frame lacking its requested depth");
  require(product_bit(static_cast<ProductKind>(255U)) == 0U, "unknown kind made an invalid shift");
}

}  // namespace

int main() {
  try {
    test_selective_copy_and_generation();
    test_pinned_slots_are_never_recycled();
    test_limits_and_identity_rejections();
    test_extended_selective_publication_hides_unavailable_products();
    std::cout << "ipc_v2_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "ipc_v2_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
