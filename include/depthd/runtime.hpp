#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "depthd/capture.hpp"
#include "depthd/rectification_backend.hpp"

namespace depthd {

// A request retains paired camera leases and any acquired CPU mappings for the
// complete asynchronous processing operation. Capture-native requests may
// defer those mappings until a CPU rectifier/fallback actually needs them.
struct StereoProcessingRequest {
  StereoPair capture{};
  std::shared_ptr<const void> left_mapping_owner{};
  std::shared_ptr<const void> right_mapping_owner{};
  ImageView left{};
  ImageView right{};
  std::optional<DmabufStereoInput> dmabuf{};
  std::shared_ptr<const DmabufRequestLease> left_dmabuf_lease{};
  std::shared_ptr<const DmabufRequestLease> right_dmabuf_lease{};

  StereoProcessingRequest() = default;
  StereoProcessingRequest(const StereoProcessingRequest&) = delete;
  StereoProcessingRequest& operator=(const StereoProcessingRequest&) = delete;
  StereoProcessingRequest(StereoProcessingRequest&&) noexcept = default;
  StereoProcessingRequest& operator=(StereoProcessingRequest&& other) noexcept {
    if (this == &other) {
      return *this;
    }

    // The default memberwise move assignment would replace capture first,
    // releasing its camera requests while the old CPU mappings and DMA-BUF
    // lease views still refer to those allocations. Tear down every dependent
    // view/owner first, then release and replace the camera leases.
    dmabuf.reset();
    right_dmabuf_lease.reset();
    left_dmabuf_lease.reset();
    right = {};
    left = {};
    right_mapping_owner.reset();
    left_mapping_owner.reset();
    capture = std::move(other.capture);

    left_mapping_owner = std::move(other.left_mapping_owner);
    right_mapping_owner = std::move(other.right_mapping_owner);
    left = other.left;
    right = other.right;
    dmabuf = std::move(other.dmabuf);
    left_dmabuf_lease = std::move(other.left_dmabuf_lease);
    right_dmabuf_lease = std::move(other.right_dmabuf_lease);
    return *this;
  }
};

struct NativePipelineConfiguration {
  std::array<std::uint8_t, 16> device_uuid{};
  std::array<std::uint8_t, 16> run_uuid{};
  std::uint64_t configuration_epoch{1U};
  RectifiedCalibrationView image_calibration{};
  RectifiedCalibrationView matching_calibration{};
  DepthConversionOptions depth{};
};

struct NativePipelineStats {
  RectificationStats rectification{};
  StereoBackendStats stereo{};
  DepthConversionStats depth{};
  double total_ms{};
};

// Owns every product in a coherent frame set. Views returned by view() remain
// valid for this object's lifetime and never retain the camera request.
struct FrameSetStorage {
  ProductSetMetadata metadata{};
  std::string rectification_backend;
  std::string rectification_fallback_reason;
  std::string stereo_backend;
  NativePipelineStats stats{};
  std::vector<std::uint8_t> left_rectified;
  std::vector<std::uint8_t> right_rectified;
  std::vector<float> disparity;
  std::vector<float> depth_m;
  std::vector<std::uint8_t> confidence;
  std::vector<std::uint8_t> validity;

  [[nodiscard]] ProductSetView view() const;
  [[nodiscard]] std::size_t payload_bytes() const noexcept;
};

class NativeProcessingPipeline final {
 public:
  NativeProcessingPipeline(
      NativePipelineConfiguration configuration,
      std::unique_ptr<RectificationBackend> rectifier,
      std::unique_ptr<NativeDisparityBackend> stereo);

  NativeProcessingPipeline(const NativeProcessingPipeline&) = delete;
  NativeProcessingPipeline& operator=(const NativeProcessingPipeline&) =
      delete;

  [[nodiscard]] std::shared_ptr<const FrameSetStorage> process(
      StereoProcessingRequest request);

 private:
  NativePipelineConfiguration configuration_{};
  std::unique_ptr<RectificationBackend> rectifier_;
  std::unique_ptr<NativeDisparityBackend> stereo_;
};

struct SchedulerConfiguration {
  std::size_t pending_capacity{2U};
  std::size_t completed_capacity{2U};
};

enum class SubmitResult : std::uint8_t {
  kAccepted = 0,
  kQueueFull = 1,
  kStopped = 2,
};

struct SchedulerStats {
  std::uint64_t accepted{};
  std::uint64_t input_rejected{};
  std::uint64_t pending_replaced{};
  std::uint64_t processed{};
  std::uint64_t processing_failures{};
  std::uint64_t completed_dropped{};
  std::size_t pending{};
  std::size_t completed{};
  bool running{};
};

// One worker owns the stateful rectifier and SGBM backend. Both queues are
// bounded. When completed output is full, the oldest complete frame set is
// discarded atomically; individual products are never mixed or dropped.
class BoundedNativeScheduler final {
 public:
  BoundedNativeScheduler(
      SchedulerConfiguration configuration,
      std::unique_ptr<NativeProcessingPipeline> pipeline);
  ~BoundedNativeScheduler() noexcept;

  BoundedNativeScheduler(const BoundedNativeScheduler&) = delete;
  BoundedNativeScheduler& operator=(const BoundedNativeScheduler&) = delete;

  [[nodiscard]] SubmitResult try_submit(StereoProcessingRequest request);
  [[nodiscard]] std::shared_ptr<const FrameSetStorage> wait_next(
      std::chrono::milliseconds timeout);
  [[nodiscard]] SchedulerStats stats() const noexcept;
  [[nodiscard]] std::string take_last_error();
  void stop() noexcept;

 private:
  void worker_loop() noexcept;

  SchedulerConfiguration configuration_{};
  std::unique_ptr<NativeProcessingPipeline> pipeline_;
  mutable std::mutex mutex_;
  std::condition_variable input_ready_;
  std::condition_variable output_ready_;
  std::deque<StereoProcessingRequest> pending_;
  std::deque<std::shared_ptr<const FrameSetStorage>> completed_;
  SchedulerStats stats_{};
  std::string last_error_;
  bool stop_requested_{};
  std::thread worker_;
};

void validate(const StereoProcessingRequest& request);
void validate(const NativePipelineConfiguration& configuration);

// Describes the first (luma) DMA-BUF plane for capture-native GPU import and
// installs a lazy mapper with explicit Linux DMA_BUF_SYNC read bracketing for
// CPU rectification/fallback. The request owns import lease views and both
// camera leases. It throws on non-Linux hosts or unsupported formats.
[[nodiscard]] StereoProcessingRequest make_cpu_dmabuf_processing_request(
    StereoPair pair);
[[nodiscard]] bool cpu_dmabuf_mapping_available() noexcept;

struct CpuDmabufMappingStats {
  std::uint64_t mappings_started{};
  std::uint64_t sync_start_retries{};
  std::uint64_t sync_start_failures{};
  std::uint64_t sync_end_retries{};
  std::uint64_t sync_end_failures{};
};

// Process-lifetime counters make otherwise-noexcept DMA-BUF cleanup failures
// visible to status/diagnostic callers.
[[nodiscard]] CpuDmabufMappingStats cpu_dmabuf_mapping_stats() noexcept;

}  // namespace depthd
