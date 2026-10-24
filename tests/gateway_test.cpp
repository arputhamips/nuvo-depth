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
  depthd::gateway::SessionQueue views(1024U);
  auto owner = std::make_shared<const depthd::gateway::SerializedBytes>(bytes);
  std::weak_ptr<const depthd::gateway::SerializedBytes> lifetime = owner;
  const auto payload = owner->prefix().subspan(16U, 32U);
  auto selected = std::make_shared<const depthd::gateway::SerializedBytes>(
      std::vector<std::byte>(8U, std::byte{3}), owner,
      std::vector<std::span<const std::byte>>{payload});
  require(selected->parts()[1].data() == payload.data() && selected->size() == 40U &&
      selected->retained_size() == 136U, "selected payload was copied or backing memory was undercounted");
  require(views.offer_buffered(selected) == depthd::gateway::OfferResult::kAccepted,
      "buffered selection refused");
  selected.reset(); owner.reset();
  auto transmitting = views.begin_buffered_transmission();
  require(!lifetime.expired() && transmitting->copy().at(8U) == 7U,
      "queued selection lost its immutable source owner");
  views.note_bytes_sent(40U); views.complete_transmission(); transmitting.reset();
  require(lifetime.expired(), "completed segmented transmission retained source owner");
  auto source = std::make_shared<const depthd::gateway::SerializedBytes>(bytes);
  auto prefix_and_payload = std::make_shared<const depthd::gateway::SerializedBytes>(
      std::vector<std::byte>(8U, std::byte{3}), source,
      std::vector<std::span<const std::byte>>{source->prefix().subspan(16U, 32U)});
  depthd::gateway::SessionQueue copying_api(1024U);
  (void)copying_api.offer_buffered(prefix_and_payload);
  const auto flattened = copying_api.begin_serialized_transmission();
  require(flattened && flattened->size() == 40U && flattened->at(0U) == std::byte{3} &&
          flattened->at(8U) == std::byte{7}, "legacy vector transmission returned only a segmented prefix");
  copying_api.note_bytes_sent(flattened->size()); copying_api.complete_transmission();
  auto numeric = std::make_shared<const depthd::gateway::SerializedBytes>(std::vector<std::byte>(20U,std::byte{4}));
  auto preview = std::make_shared<const depthd::gateway::SerializedBytes>(std::vector<std::byte>(10U,std::byte{5}));
  std::weak_ptr<const depthd::gateway::SerializedBytes> numeric_lifetime = numeric, preview_lifetime = preview;
  auto pair = std::make_shared<const depthd::gateway::SerializedBytes>(std::vector<std::byte>(32U,std::byte{1}),
      std::vector<std::shared_ptr<const depthd::gateway::SerializedBytes>>{numeric,preview});
  require(pair->parts()[1].data() == numeric->data() && pair->parts()[2].data() == preview->data() &&
          pair->size() == 62U && pair->retained_size() == 62U, "paired output copied payload or undercounted retained owners");
  numeric.reset(); preview.reset();
  require(!numeric_lifetime.expired() && !preview_lifetime.expired() && pair->copy().at(52U) == 5U,
          "paired segmented output lost either immutable owner");
  pair.reset();
  require(numeric_lifetime.expired() && preview_lifetime.expired(), "paired output retained owners after release");
}

void test_independent_wifi_permission_preserves_wired_policy() {
  using depthd::gateway::TransportKind;
  using depthd::gateway::TransportPreference;
  for (const auto preference : {TransportPreference::kEthernetPreferred,
       TransportPreference::kUsbPreferred, TransportPreference::kBoth,
       TransportPreference::kAuto, TransportPreference::kWifiOnly,
       TransportPreference::kOff}) {
    auto config = configuration(preference);
    config.wifi = {true, true, 8U * 1024U * 1024U, 16U * 1024U * 1024U, 4U};
    depthd::gateway::AdmissionController controller(config);
    const bool legacy_wifi = preference == TransportPreference::kAuto ||
                             preference == TransportPreference::kWifiOnly;
    require(controller.admit(request(1000U, 1.0), TransportKind::kWifi).lease.has_value() == legacy_wifi,
            "omitting Wi-Fi permission changed legacy transport admission");
    for (const bool enabled : {false, true}) {
      const bool allowed = enabled && preference != TransportPreference::kOff;
      require(controller.admit(request(1000U, 1.0), TransportKind::kWifi, preference, enabled).lease.has_value() == allowed,
              "Wi-Fi permission did not apply independently or bypassed master off");
      require(depthd::gateway::transport_permitted(preference, TransportKind::kWifi, enabled) == allowed,
              "reported Wi-Fi eligibility disagrees with admission");
      for (const auto kind : {TransportKind::kEthernet, TransportKind::kUsbEthernet}) {
        const bool before = controller.admit(request(1000U, 1.0), kind, preference).lease.has_value();
        const bool after = controller.admit(request(1000U, 1.0), kind, preference, enabled).lease.has_value();
        require(before == after, "Wi-Fi-only patch changed wired admission preference");
      }
    }
  }
  auto config = configuration(TransportPreference::kEthernetPreferred);
  config.wifi = {true, true, 8U * 1024U * 1024U, 16U * 1024U * 1024U, 4U};
  config.wifi_enabled = true;
  depthd::gateway::AdmissionController controller(config);
  require(controller.admit(request(1000U, 1.0), TransportKind::kWifi).lease.has_value(),
          "stored independent Wi-Fi permission was ignored");
  require(!controller.admit(request(1000U, 1.0), TransportKind::kWifi, {}, false).lease,
          "explicit false did not override configured Wi-Fi permission");
  require(!depthd::gateway::transport_permitted(static_cast<TransportPreference>(255), TransportKind::kWifi, true),
          "invalid preference was treated as permitted Wi-Fi");
}

void test_absent_preferred_interface_does_not_block_connected_wired_transport() {
  using depthd::gateway::TransportKind;
  using depthd::gateway::TransportPreference;
  depthd::gateway::AdmissionController controller(configuration(TransportPreference::kUsbPreferred));
  const auto ethernet_only = depthd::gateway::transport_bit(TransportKind::kEthernet);
  auto wired = controller.admit(request(1000U, 1.0), TransportKind::kEthernet, {}, {}, ethernet_only);
  require(wired.lease.has_value(), "absent USB interface blocked a connected Ethernet client");
  auto prefer_usb = controller.admit(request(1000U, 1.0), TransportKind::kEthernet, {}, {}, depthd::gateway::kAllTransports);
  require(!prefer_usb.lease && prefer_usb.compatible_alternatives.size() == 1U &&
          prefer_usb.compatible_alternatives.front() == TransportKind::kUsbEthernet,
          "available preferred USB stopped enforcing wired preference");
  require(wired.lease->transport() == TransportKind::kEthernet && controller.stats().ethernet_sessions == 1U,
          "availability update invalidated an already admitted lease");
  auto impossible = controller.admit(request(8'000'000U, 60.0), TransportKind::kEthernet, {}, {}, 0U);
  require(!impossible.lease && impossible.compatible_alternatives.empty(),
          "admission advertised a nonexistent reconnect alternative");
}

void test_encoded_wifi_is_independent_of_raw_permission_and_shares_capacity() {
  using namespace depthd::gateway;
  for (const auto preference : {TransportPreference::kEthernetPreferred,
       TransportPreference::kUsbPreferred, TransportPreference::kBoth,
       TransportPreference::kAuto, TransportPreference::kWifiOnly,
       TransportPreference::kOff}) {
    auto config = configuration(preference);
    config.wifi = {true, true, 1'000'000U, 1'000'000U, 2U};
    config.wifi_enabled = false;
    AdmissionController controller(config);
    auto encoded = request(1000U, 5.0);
    encoded.encoded = true;
    const bool allowed = preference != TransportPreference::kOff;
    auto preview = controller.admit(encoded, TransportKind::kWifi);
    require(preview.lease.has_value() == allowed,
            "raw Wi-Fi disabled encoded admission or encoded bypassed master off");
    require(transport_permitted(preference, TransportKind::kWifi, false, true) == allowed,
            "encoded permission does not match admission");
    require(!controller.admit(request(1000U, 5.0), TransportKind::kWifi).lease,
            "encoded eligibility leaked to raw Wi-Fi admission");
    if (!allowed) continue;
    auto raw = controller.admit(request(1000U, 5.0), TransportKind::kWifi, {}, true);
    require(raw.lease && controller.stats().wifi_sessions == 2U,
            "raw and encoded cannot share the same Wi-Fi link");
    require(!controller.admit(encoded, TransportKind::kWifi).lease,
            "encoded streams bypassed the shared session budget");
    raw.lease.reset();
    require(preview.lease && controller.stats().wifi_sessions == 1U,
            "releasing raw demand released the encoded lease");
    auto oversized_rate = request(300'000U, 5.0); oversized_rate.encoded = true;
    require(!controller.admit(oversized_rate, TransportKind::kWifi).lease,
            "encoded Wi-Fi bypassed the shared byte budget");
    require(!transport_permitted(preference, TransportKind::kWifi, false) &&
            transport_permitted(preference, TransportKind::kWifi, false, true),
            "raw-only policy change cannot revalidate raw and encoded independently");
  }
  auto insecure = configuration(TransportPreference::kAuto);
  insecure.wifi = {true, false, 1'000'000U, 1'000'000U, 2U};
  bool rejected{};
  try { AdmissionController controller(insecure); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "encoded Wi-Fi bypassed TLS requirements");
  require(!transport_permitted(static_cast<TransportPreference>(255), TransportKind::kWifi, false, true),
          "encoded Wi-Fi bypassed malformed policy");
}

void test_device_mode_is_not_inferred_from_usb_host_networking() {
  using depthd::gateway::network_path;
  require(network_path(false, false, "/sys/devices/platform/1000480000.usb/gadget", "", false) == "usb_device",
          "gadget classification incorrectly depended on a /usb ancestry component");
  require(network_path(false, false, "/sys/devices/platform/axi/usb/gadget", "", false) == "usb_device",
          "kernel gadget device evidence was ignored");
  require(network_path(false, false, "", "/sys/devices/platform/usb/gadget/net/usb0", false) == "usb_device",
          "kernel gadget netdev evidence was ignored");
  require(network_path(false, false, "/sys/devices/platform/axi/1000480000.usb/gadget.0", "", false) == "usb_device",
          "Raspberry Pi g_ether gadget.0 device was mislabeled Ethernet");
  require(network_path(false, false, "", "/sys/devices/platform/axi/1000480000.usb/gadget.12/net/renamed0", false) == "usb_device",
          "numbered gadget netdev must not depend on its interface name");
  for (const auto suffix : {"gadget.", "gadget.0suffix", "gadgetry"}) {
    const auto device = std::string("/sys/devices/platform/controller/") + suffix;
    require(network_path(false, false, device, "", false) != "usb_device" &&
            network_path(false, false, "", device + "/net/usb0", false) != "usb_device",
            "a gadget-like name is not a numbered kernel gadget device");
  }
  require(network_path(false, false, "", "", true) == "usb_device",
          "bound configfs network-function evidence was ignored");
  require(network_path(false, false, "/sys/devices/pci/usb1/1-1", "", false) == "usb_host_adapter",
          "USB host adapter was mislabeled USB-C device mode");
  require(network_path(false, true, "/sys/devices/pci/usb1/1-1", "", false) == "wifi",
          "USB wireless adapter was mislabeled USB Ethernet");
  require(network_path(true, false, "", "", false) == "other" &&
          network_path(false, false, "", "/sys/devices/virtual/net/eth0", false) == "other",
          "interface name or virtual interface was treated as physical port evidence");
  require(network_path(false, false, "/sys/devices/platform/ethernet", "", false) == "ethernet",
          "physical Ethernet evidence was ignored");
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
    test_independent_wifi_permission_preserves_wired_policy();
    test_encoded_wifi_is_independent_of_raw_permission_and_shares_capacity();
    test_absent_preferred_interface_does_not_block_connected_wired_transport();
    test_device_mode_is_not_inferred_from_usb_host_networking();
    test_serialized_storage_is_shared_and_complete();
    std::cout << "gateway_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "gateway_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
