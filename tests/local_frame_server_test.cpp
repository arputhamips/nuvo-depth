#include "depthd/local_frame_server.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <future>
#include <array>
#include <cstring>
#include <thread>

#include <unistd.h>
#include <sys/stat.h>

namespace {

std::shared_ptr<depthd::FrameSetStorage> frame() {
  auto result = std::make_shared<depthd::FrameSetStorage>();
  result->metadata.frame.device_uuid[0] = 1U;
  result->metadata.frame.run_uuid[0] = 2U;
  result->metadata.frame.capture_epoch = 1U;
  result->metadata.frame.capture_sequence = 7U;
  result->metadata.frame.configuration_epoch = 1U;
  result->metadata.frame.capture_time_ns = 1;
  result->metadata.frame.capture_clock = depthd::CaptureClock::kBoottime;
  result->metadata.calibration.sha256[0] = 1U;
  const auto grid = depthd::make_reference_grid(2U, 1U);
  const depthd::RegionU32 roi{0U, 0U, 2U, 1U};
  result->metadata.left_rectified = {
      grid, roi, depthd::CameraRole::kLogicalLeft,
      depthd::ImageProductFormat::kGray8};
  result->metadata.right_rectified = {
      grid, roi, depthd::CameraRole::kLogicalRight,
      depthd::ImageProductFormat::kGray8};
  result->metadata.disparity = {
      grid, roi, 0.5F, 8.0F, 0.0625F, true,
      depthd::ConfidenceModel::kBinarySgbmSupportV1,
      depthd::DisparityDerivation::kNativeMatcherGrid};
  result->metadata.depth = {grid, 0.1F, 10.0F};
  result->left_rectified = {1U, 2U};
  result->right_rectified = {3U, 4U};
  result->disparity = {1.0F, 2.0F};
  result->depth_m = {3.0F, 4.0F};
  result->confidence = {255U, 255U};
  result->validity = {1U, 1U};
  return result;
}

}  // namespace

int main() {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("nuvo-local-frame-" + std::to_string(::getpid()));
  std::filesystem::create_directory(directory);
  const auto socket = directory / "data.sock";
  {
    depthd::ipc_v2::LocalFrameServer server(socket, 0600U);
    server.publish(frame());
    const auto message = depthd::ipc_v2::request_latest_local(
        socket, depthd::ipc_v2::product_bit(
                    depthd::ipc_v2::ProductKind::kDepth));
    if (message.header.kind != depthd::wire::MessageKind::kFrameSet ||
        message.header.stream_sequence != 7U || message.payload.size() != 8U) {
      return 1;
    }
    const auto first = depthd::ipc_v2::request_next_local(
        socket, depthd::ipc_v2::kAllProducts);
    if (!first || first->cursor.capture_sequence != 7U ||
        depthd::wire::deserialize(first->bytes).payload.size() != 24U) return 3;
    const auto exported = [&] {
      auto connection = depthd::ipc_v2::connect_seqpacket(socket);
      const std::array request{std::byte{'N'}, std::byte{'V'}, std::byte{'Q'},
          std::byte{'2'}, std::byte{0}, std::byte{0}, std::byte{0},
          static_cast<std::byte>(depthd::ipc_v2::kAllProducts)};
      connection.send_packet(request);
      auto response = connection.receive_packet(16U, 1U, std::chrono::seconds(1));
      return std::move(response.handles.at(0));
    };
    auto original_handle = exported();
    auto duplicate_handle = exported();
    struct stat original_info{}, duplicate_info{};
    if (::fstat(original_handle.get(), &original_info) != 0 ||
        ::fstat(duplicate_handle.get(), &duplicate_info) != 0 ||
        original_info.st_ino != duplicate_info.st_ino) return 8;
    auto waiting = std::async(std::launch::async, [&] {
      return depthd::ipc_v2::request_next_local(socket,
          depthd::ipc_v2::kAllProducts, first->cursor, std::chrono::milliseconds(500));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const auto began = std::chrono::steady_clock::now();
    (void)depthd::ipc_v2::request_latest_local(socket);
    if (std::chrono::steady_clock::now() - began > std::chrono::milliseconds(200)) return 4;
    // A daemon restart can reuse epoch/sequence; run UUID still wakes readers.
    auto restarted = frame();
    restarted->metadata.frame.run_uuid[0] = 3U;
    restarted->depth_m[0] = 9.0F;
    server.publish(restarted);
    const auto next = waiting.get();
    if (!next || next->cursor.capture_sequence != 7U ||
        next->cursor.run_uuid[0] != 3U || next->bytes == first->bytes) return 5;
    // Existing exported bytes remain immutable even after the producer pool
    // advances; caching never overwrites a previously handed-out memfd.
    std::vector<std::byte> retained(first->bytes.size());
    if (::pread(original_handle.get(), retained.data(), retained.size(), 0) !=
        static_cast<ssize_t>(retained.size()) || retained != first->bytes) return 9;
    if (depthd::ipc_v2::request_next_local(socket, depthd::ipc_v2::kAllProducts,
            next->cursor, std::chrono::milliseconds(20))) return 6;
    auto stopped_waiter = std::async(std::launch::async, [&] {
      return depthd::ipc_v2::request_next_local(socket,
          depthd::ipc_v2::kAllProducts, next->cursor, std::chrono::milliseconds(500));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const auto stopped_at = std::chrono::steady_clock::now();
    server.stop();
    if (stopped_waiter.get() || std::chrono::steady_clock::now() - stopped_at >
            std::chrono::milliseconds(300)) return 7;
  }
  {
    using namespace depthd::ipc_v2;
    LocalFrameServer server(socket, 0600U);
    server.publish(frame());
    if (server.demand().products != 0U || server.demand().cadence_hz != 0.0) return 10;
    const auto depth = product_bit(ProductKind::kDepth);
    const auto rgb = product_bit(ProductKind::kRightRgb);
    const auto mono = product_bit(ProductKind::kLeftMono);
    {
      LocalFrameClient first(socket, depth, 7.0, 4.0);
      const auto initial = server.demand();
      if (initial.products != depth || initial.cadence_hz != 7.0 ||
          initial.preview_cadence_hz != 4.0) return 11;
      const auto selected = first.next();
      if (!selected || depthd::wire::deserialize(selected->bytes).payload.size() != 8U) return 12;
      LocalFrameClient second(socket, mono | rgb, 12.0, 0.0);
      auto demand = server.demand();
      if (demand.products != (depth | mono | rgb) || demand.cadence_hz != 12.0 ||
          demand.preview_cadence_hz != 4.0 || demand.generation <= initial.generation) return 13;
      first.set_demand(rgb, 8.0, 8.0);
      second.set_demand(0U, 0.0, 0.0);
      demand = server.demand();
      if (demand.products != rgb || demand.cadence_hz != 8.0 || demand.preview_cadence_hz != 8.0) return 14;
      first.set_demand(rgb, 8.0, 8.0);
      if (server.demand().generation != demand.generation) return 15;
      auto waiting = std::async(std::launch::async, [&] {
        return first.next(selected->cursor, std::chrono::milliseconds(500));
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      // A cached frame without the requested product cannot satisfy demand.
      if (waiting.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) return 16;
      auto selective = frame();
      selective->metadata.frame.capture_sequence = 8U;
      selective->available_products = rgb;
      selective->left_rectified.clear(); selective->right_rectified.clear();
      selective->disparity.clear(); selective->depth_m.clear();
      selective->confidence.clear(); selective->validity.clear();
      selective->right_rgb = {12U, 23U, 34U, 45U, 56U, 67U};
      server.publish(selective);
      const auto next = waiting.get();
      if (!next || next->cursor.capture_sequence != 8U ||
          depthd::wire::deserialize(next->bytes).payload.size() != 6U) return 17;
      if (first.next(next->cursor, std::chrono::milliseconds(10)) || server.demand().products != rgb) return 18;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (server.demand().products != 0U && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (server.demand().products != 0U || server.demand().preview_cadence_hz != 0.0) return 19;
    // A preview-only client contributes no raw work, and idle persistent
    // sockets do not make server shutdown wait for their receive timeout.
    LocalFrameClient preview(socket, 0U, 0.0, 10.0);
    if (server.demand().products != 0U || server.demand().preview_cadence_hz != 10.0) return 20;
    const auto stopped = std::chrono::steady_clock::now();
    server.stop();
    if (std::chrono::steady_clock::now() - stopped > std::chrono::milliseconds(300)) return 21;
  }
  {
    const auto started = std::chrono::steady_clock::now();
    {
      depthd::ipc_v2::LocalFrameServer server(socket, 0600U);
      auto stalled_client = depthd::ipc_v2::connect_seqpacket(socket);
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (std::chrono::steady_clock::now() - started >
        std::chrono::seconds(1)) {
      return 2;
    }
  }
  std::filesystem::remove(directory);
}
