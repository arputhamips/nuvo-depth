#include "depthd/local_frame_server.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace depthd;

std::shared_ptr<FrameSetStorage> frame(std::uint64_t sequence) {
  auto result = std::make_shared<FrameSetStorage>();
  result->metadata.frame.device_uuid[0] = 1U;
  result->metadata.frame.run_uuid[0] = 2U;
  result->metadata.frame.capture_epoch = 1U;
  result->metadata.frame.capture_sequence = sequence;
  result->metadata.frame.configuration_epoch = 1U;
  result->metadata.frame.capture_time_ns = sequence;
  result->metadata.frame.capture_clock = CaptureClock::kBoottime;
  result->metadata.calibration.sha256[0] = 1U;
  const auto grid = make_reference_grid(2U, 1U);
  const RegionU32 roi{0U, 0U, 2U, 1U};
  result->metadata.left_rectified = {grid, roi, CameraRole::kLogicalLeft, ImageProductFormat::kGray8};
  result->metadata.right_rectified = {grid, roi, CameraRole::kLogicalRight, ImageProductFormat::kGray8};
  result->metadata.disparity = {grid, roi, 0.5F, 8.0F, 0.0625F, true,
      ConfidenceModel::kBinarySgbmSupportV1, DisparityDerivation::kNativeMatcherGrid};
  result->metadata.depth = {grid, 0.1F, 10.0F};
  result->left_rectified = {1U, 2U}; result->right_rectified = {3U, 4U};
  result->disparity = {1.0F, 2.0F}; result->depth_m = {3.0F, 4.0F};
  result->confidence = {255U, 255U}; result->validity = {1U, 0U};
  return result;
}

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  const auto base = std::filesystem::temp_directory_path() / ("nuvo-cli-" + std::to_string(getpid()));
  std::filesystem::create_directory(base);
  const auto socket = (base/"data.sock").string();
  const auto output = (base/"depth.nuv").string();
  int result{};
  {
    ipc_v2::LocalFrameServer server(socket);
    server.publish(frame(1U));
    const auto child = fork();
    if (child == 0) {
      const int file = open(output.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (file < 0 || dup2(file, STDOUT_FILENO) < 0) _exit(90);
      close(file);
      execl(argv[1], argv[1], "--socket", socket.c_str(), "--frames", "2", "--timeout-seconds", "2", nullptr);
      _exit(91);
    }
    if (child < 0) return 2;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!server.demand().products && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    const auto expected = ipc_v2::product_bit(ipc_v2::ProductKind::kDepth) | ipc_v2::product_bit(ipc_v2::ProductKind::kValidity);
    if (server.demand().products != expected || server.demand().preview_cadence_hz != 0.0) result = 3;
    // Keep publishing fresh identities until the finite reader exits. The
    // cached sequence 1 must never appear in its exported bytes.
    int status{}; bool finished{};
    for (std::uint64_t sequence = 2U; sequence < 30U; ++sequence) {
      std::this_thread::sleep_for(30ms); server.publish(frame(sequence));
      if (waitpid(child, &status, WNOHANG) == child) { finished = true; break; }
    }
    if (!finished) { kill(child, SIGKILL); waitpid(child, &status, 0); result = 4; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) result = 5;
    std::ifstream file(output, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)), {});
    std::span bytes(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
    std::size_t count{};
    while (!bytes.empty()) {
      const auto header = wire::decode_header(bytes.first(wire::kHeaderBytes));
      const auto length = wire::kHeaderBytes + header.metadata_bytes + header.payload_bytes;
      const auto message = wire::deserialize(bytes.first(length));
      const std::string metadata(reinterpret_cast<const char*>(message.metadata.data()), message.metadata.size());
      if (message.payload.size() != 10U || metadata.find("\"capture_sequence\":1,") != std::string::npos) result = 6;
      bytes = bytes.subspan(length); ++count;
    }
    if (count != 2U) result = 7;
    deadline = std::chrono::steady_clock::now()+1s;
    while (server.demand().products && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
    if (server.demand().products) result = 8;
  }
  std::filesystem::remove_all(base);
  return result;
}
