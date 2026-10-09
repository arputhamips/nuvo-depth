#include "depthd/gateway.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

depthd::gateway::AdmissionConfiguration configuration(
    const depthd::gateway::TransportPreference preference) {
  depthd::gateway::AdmissionConfiguration result;
  result.preference = preference;
  result.ethernet = {true, true, 80U * 1000U * 1000U,
                     64U * 1024U * 1024U, 4U};
  result.usb_ethernet = {true, true, 25U * 1000U * 1000U,
                        25U * 1024U * 1024U, 2U};
  return result;
}

depthd::gateway::SubscriptionRequest request(const std::uint64_t frame_bytes,
                                              const double hz) {
  return {depthd::ipc_v2::kAllProducts, frame_bytes, 1024U, hz, false, false};
}

std::shared_ptr<const depthd::wire::Message> message(
    const std::uint64_t sequence, const std::size_t payload = 16U) {
  auto result = std::make_shared<depthd::wire::Message>();
  result->header.kind = depthd::wire::MessageKind::kFrameSet;
  result->header.stream_sequence = sequence;
  result->metadata.assign(4U, std::byte{0x11});
  result->payload.assign(payload, std::byte{0x22});
  result->header.metadata_bytes = 4U;
  result->header.payload_bytes = payload;
  return result;
}

void test_exact_admission_and_transport_policy() {
  depthd::gateway::AdmissionController ethernet_preferred(
      configuration(depthd::gateway::TransportPreference::kEthernetPreferred));
  auto admitted = ethernet_preferred.admit(request(1'780'000U, 10.0));
  require(admitted.lease.has_value() &&
              admitted.lease->transport() ==
                  depthd::gateway::TransportKind::kEthernet,
          "balanced stream did not follow Ethernet preference");
  require(admitted.lease->reserved_bytes_per_second() == 17'810'560U,
          "admission did not reserve exact framed bandwidth");
  admitted.lease->reset();
  require(ethernet_preferred.stats().ethernet_reserved_bytes_per_second == 0U,
          "admission lease did not release its reservation");

  depthd::gateway::AdmissionController usb_preferred(
      configuration(depthd::gateway::TransportPreference::kUsbPreferred));
  auto usb = usb_preferred.admit(request(1'780'000U, 10.0));
  require(usb.lease.has_value() &&
              usb.lease->transport() ==
                  depthd::gateway::TransportKind::kUsbEthernet,
          "balanced stream did not follow USB preference");
  auto ethernet_fallback = usb_preferred.admit(request(4'000'000U, 10.0));
  require(ethernet_fallback.lease.has_value() &&
              ethernet_fallback.lease->transport() ==
                  depthd::gateway::TransportKind::kEthernet,
          "request exceeding USB qualification did not use Ethernet");

  depthd::gateway::AdmissionController disabled(
      configuration(depthd::gateway::TransportPreference::kOff));
  auto rejected = disabled.admit(request(1000U, 1.0));
  require(!rejected.lease.has_value() &&
              rejected.reason.find("disabled") != std::string::npos,
          "off policy admitted a data stream");
}

void test_no_silent_degradation_or_unqualified_tls() {
  auto config = configuration(
      depthd::gateway::TransportPreference::kEthernetPreferred);
  config.ethernet.qualified_payload_bytes_per_second = 10'000U;
  config.usb_ethernet.qualified_payload_bytes_per_second = 10'000U;
  depthd::gateway::AdmissionController controller(config);
  auto impossible = request(1'000'000U, 30.0);
  impossible.allow_cadence_reduction = true;
  impossible.allow_format_change = true;
  const auto rejected = controller.admit(impossible);
  require(!rejected.lease.has_value(),
          "gateway silently reduced an exact subscription contract");

  bool rejected_insecure = false;
  try {
    auto insecure = configuration(
        depthd::gateway::TransportPreference::kEthernetPreferred);
    insecure.ethernet.tls_enabled = false;
    (void)depthd::gateway::AdmissionController(insecure);
  } catch (const std::invalid_argument&) {
    rejected_insecure = true;
  }
  require(rejected_insecure,
          "enabled remote transport was accepted without TLS");
}

void test_whole_frame_backpressure_and_partial_close() {
  depthd::gateway::SessionQueue queue(4096U);
  require(queue.offer(message(1U)) == depthd::gateway::OfferResult::kAccepted,
          "first complete frame was rejected");
  require(queue.offer(message(2U)) ==
              depthd::gateway::OfferResult::kReplacedPendingComplete,
          "old unsent frame was not atomically replaced");
  const auto sending = queue.begin_transmission();
  require(sending && sending->header.stream_sequence == 2U,
          "session began the wrong complete frame");
  require(queue.offer(message(3U)) == depthd::gateway::OfferResult::kAccepted,
          "one pending frame was not allowed beside in-flight data");
  queue.note_bytes_sent(1U);
  require(!queue.begin_transmission(),
          "session interleaved a new frame into an active frame");
  bool refused_partial_complete = false;
  try {
    queue.complete_transmission();
  } catch (const std::logic_error&) {
    refused_partial_complete = true;
  }
  require(refused_partial_complete,
          "session marked a partial TCP frame complete");
  queue.close_incomplete();
  const auto stats = queue.stats();
  require(!stats.open && stats.pending_replaced == 1U &&
              stats.sessions_closed_incomplete == 1U,
          "partial-frame close or whole-frame drop accounting failed");
  require(queue.offer(message(4U)) == depthd::gateway::OfferResult::kClosed,
          "closed stream session accepted another frame");
}

void test_complete_transmission() {
  depthd::gateway::SessionQueue queue(4096U);
  auto offered = message(1U);
  require(queue.offer(offered) == depthd::gateway::OfferResult::kAccepted,
          "valid frame was rejected");
  const auto sending = queue.begin_transmission();
  queue.note_bytes_sent(sending->wire_bytes());
  queue.complete_transmission();
  require(queue.stats().transmissions_completed == 1U,
          "complete transmission was not accounted");
}

void test_connection_cannot_reserve_another_interface() {
  auto config = configuration(depthd::gateway::TransportPreference::kEthernetPreferred);
  config.usb_ethernet.qualified_payload_bytes_per_second = 1'000U;
  depthd::gateway::AdmissionController controller(config);
  auto refused = controller.admit(request(1'000'000U, 10.0),
      depthd::gateway::TransportKind::kUsbEthernet);
  require(!refused.lease && refused.compatible_alternatives.size() == 1U &&
      refused.compatible_alternatives.front() == depthd::gateway::TransportKind::kEthernet,
      "USB connection silently consumed Ethernet capacity");
  auto accepted = controller.admit(request(1'000'000U, 10.0),
      depthd::gateway::TransportKind::kEthernet);
  require(accepted.lease && controller.stats().usb_sessions == 0U,
      "connected-interface admission did not account Ethernet exactly");
  depthd::gateway::AdmissionController preference_controller(configuration(
      depthd::gateway::TransportPreference::kBoth));
  auto reconnect = preference_controller.admit(request(1'000'000U, 10.0),
      depthd::gateway::TransportKind::kEthernet,
      depthd::gateway::TransportPreference::kUsbPreferred);
  require(!reconnect.lease && reconnect.compatible_alternatives.size() == 1U &&
      reconnect.compatible_alternatives.front() == depthd::gateway::TransportKind::kUsbEthernet,
      "connected-interface admission ignored current USB preference");
}

void test_wifi_resource_admission_and_selection() {
  auto config = configuration(depthd::gateway::TransportPreference::kAuto);
  config.wifi = {true, true, 8U * 1024U * 1024U, 16U * 1024U * 1024U, 4U};
  depthd::gateway::AdmissionController controller(config);
  auto image = request(728U * 544U * 3U, 5.0);
  image.products = depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kLeftRgb);
  auto admitted = controller.admit(image, depthd::gateway::TransportKind::kWifi);
  require(admitted.lease && admitted.lease->transport() == depthd::gateway::TransportKind::kWifi &&
      controller.stats().ethernet_sessions == 0U && controller.stats().wifi_sessions == 1U,
      "automatic policy rejected image-only WiFi or charged wired capacity");
  auto refused = controller.admit(image, depthd::gateway::TransportKind::kWifi);
  require(!refused.lease, "WiFi aggregate byte budget was oversubscribed");
  admitted.lease->reset();
  auto wired = controller.admit(image, depthd::gateway::TransportKind::kEthernet,
      depthd::gateway::TransportPreference::kWifiOnly);
  require(!wired.lease, "WiFi-only preference admitted an Ethernet socket");
  auto excessive = image; excessive.cadence_hz = 60;
  auto rejected = controller.admit(excessive, depthd::gateway::TransportKind::kWifi);
  require(!rejected.lease, "WiFi request silently reduced cadence to fit");
  auto snapshot = request(728U * 544U * 12U, 1.0);
  snapshot.metadata_bytes = depthd::wire::kHardMaximumMetadataBytes;
  auto one_shot = controller.admit(snapshot, depthd::gateway::TransportKind::kWifi);
  require(one_shot.lease && one_shot.lease->transport() == depthd::gateway::TransportKind::kWifi,
      "normal WiFi rejected a legacy six-product snapshot fitting its byte window");
  one_shot.lease->reset();
  snapshot.cadence_hz = 10.0;
  require(!controller.admit(snapshot, depthd::gateway::TransportKind::kWifi).lease,
      "WiFi admitted a ten-Hz legacy stream exceeding its byte budget");
}

void test_serialized_storage_is_shared_and_complete() {
  depthd::gateway::SessionQueue queue(1024U);
  auto bytes = std::make_shared<const std::vector<std::byte>>(128U, std::byte{7});
  require(queue.offer_serialized(bytes) == depthd::gateway::OfferResult::kAccepted,
      "serialized frame refused");
  auto sending = queue.begin_serialized_transmission();
  require(sending == bytes, "gateway copied immutable serialized frame for client");
  require(queue.offer_serialized(bytes) == depthd::gateway::OfferResult::kAccepted,
      "pending serialized frame refused beside in-flight frame");
  queue.note_bytes_sent(bytes->size());
  queue.complete_transmission();
  require(queue.stats().transmissions_completed == 1U && queue.stats().pending_messages == 1U,
      "serialized queue accounting lost complete frame");
  sending = queue.begin_serialized_transmission();
  queue.note_bytes_sent(1U);
  queue.close_incomplete();
  require(queue.stats().sessions_closed_incomplete == 1U,
      "partial serialized frame did not close session");
  require(queue.offer_serialized(std::make_shared<const std::vector<std::byte>>(1025U)) ==
      depthd::gateway::OfferResult::kOversize, "oversize serialized data not bounded");
}

}  // namespace

int main() {
  try {
    test_exact_admission_and_transport_policy();
    test_no_silent_degradation_or_unqualified_tls();
    test_whole_frame_backpressure_and_partial_close();
    test_complete_transmission();
    test_connection_cannot_reserve_another_interface();
    test_wifi_resource_admission_and_selection();
    test_serialized_storage_is_shared_and_complete();
    std::cout << "gateway_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "gateway_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
