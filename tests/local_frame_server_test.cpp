#include "depthd/local_frame_server.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include <unistd.h>

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
