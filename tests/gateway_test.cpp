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

}  // namespace

int main() {
  try {
    test_exact_admission_and_transport_policy();
    test_no_silent_degradation_or_unqualified_tls();
    test_whole_frame_backpressure_and_partial_close();
    test_complete_transmission();
    std::cout << "gateway_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "gateway_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
