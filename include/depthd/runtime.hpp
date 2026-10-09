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
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "depthd/capture.hpp"
#include "depthd/rectification_backend.hpp"
#include "depthd/output_selection.hpp"

namespace depthd {

enum class ColorPreviewRequest : std::uint8_t {
  kNone = 0,
  kLeft = 1,
  kRight = 2,
};

// Immutable, bounded per-pair control/telemetry snapshot for publication. A
// context must never retain camera leases or mappings. Product pool and
// scheduler capacities bound the number of snapshots retained in flight.
struct CapturePublicationContext {
  virtual ~CapturePublicationContext() = default;
};

// A request retains paired camera leases and any acquired CPU mappings for the
// complete asynchronous processing operation. Capture-native requests may
// defer those mappings until a CPU rectifier/fallback actually needs them.
struct StereoProcessingRequest {
  ipc_v2::ProductMask products{ipc_v2::kAllProducts};
  // Preserve an admitted numeric preview against ordinary pending raw work.
  // Newer previews may replace it; the queue remains bounded.
  bool preserve_for_preview{};
  StereoPair capture{};
  std::shared_ptr<const void> left_mapping_owner{};
  std::shared_ptr<const void> right_mapping_owner{};
  ImageView left{};
  ImageView right{};
  std::optional<DmabufStereoInput> dmabuf{};
  std::shared_ptr<const DmabufRequestLease> left_dmabuf_lease{};
  std::shared_ptr<const DmabufRequestLease> right_dmabuf_lease{};
  // Optional tightly-packed ISP YUV420 image copied while the capture lease is
  // live. It is monitoring-only and never enters rectification or matching.
  ColorPreviewRequest color_preview{ColorPreviewRequest::kNone};
  std::vector<std::uint8_t> color_preview_yuv420{};
  std::shared_ptr<const CapturePublicationContext> publication_context{};

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
    products = other.products;
    preserve_for_preview = other.preserve_for_preview;

    left_mapping_owner = std::move(other.left_mapping_owner);
    right_mapping_owner = std::move(other.right_mapping_owner);
    left = other.left;
    right = other.right;
    dmabuf = std::move(other.dmabuf);
    left_dmabuf_lease = std::move(other.left_dmabuf_lease);
    right_dmabuf_lease = std::move(other.right_dmabuf_lease);
    color_preview = std::exchange(other.color_preview,
                                  ColorPreviewRequest::kNone);
    color_preview_yuv420 = std::move(other.color_preview_yuv420);
    other.color_preview_yuv420.clear();
    publication_context = std::move(other.publication_context);
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
  // Slots are shared by processing, queued completions, and retained consumers.
  // Product buffers are allocated once per slot at its first use. The pipeline
  // drops complete inputs when every slot is still retained; it never expands
  // the pool or overwrites a published frame.
  std::size_t frame_pool_capacity{8U};
  // Bounds all slot product capacities (including both optional color images)
  // and the shared disparity-validity scratch. Geometry is rejected before
  // allocation if its worst-case RGB payload would exceed this budget.
  std::size_t frame_pool_payload_budget_bytes{256U * 1024U * 1024U};
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
  ipc_v2::ProductMask available_products{ipc_v2::kAllProducts};
  ProductSetMetadata metadata{};
  std::shared_ptr<const CapturePublicationContext> publication_context{};
  std::string rectification_backend;
  std::string rectification_fallback_reason;
  std::string stereo_backend;
  StereoConfigurationState stereo_configuration{};
  NativePipelineStats stats{};
  std::vector<std::uint8_t> left_rectified;
  std::vector<std::uint8_t> right_rectified;
  std::vector<std::uint8_t> left_mono;
  std::vector<std::uint8_t> right_mono;
  std::vector<std::uint8_t> left_rgb;
  std::vector<std::uint8_t> right_rgb;
  std::vector<std::uint8_t> left_rectified_rgb;
  std::vector<std::uint8_t> right_rectified_rgb;
  std::vector<std::uint8_t> rectified_stereo;
  std::vector<std::uint8_t> rectified_stereo_rgb;
  // Optional tightly-packed ISP YUV420 monitoring images. Only the requested
  // camera is retained, keeping the raw/depth-only path allocation-free.
  std::vector<std::uint8_t> left_color_yuv420;
  std::vector<std::uint8_t> right_color_yuv420;
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
      std::unique_ptr<NativeDisparityBackend> stereo,
      std::unique_ptr<RectificationBackend> rgb_rectifier = {});

  NativeProcessingPipeline(const NativeProcessingPipeline&) = delete;
  NativeProcessingPipeline& operator=(const NativeProcessingPipeline&) =
      delete;

  // One worker owns this stateful pipeline. Returns null, releasing both camera
  // leases without processing, if all fixed storage slots are consumer-pinned.
  [[nodiscard]] std::shared_ptr<const FrameSetStorage> process(
      StereoProcessingRequest request);
  [[nodiscard]] bool has_available_storage() const noexcept;

 private:
  NativePipelineConfiguration configuration_{};
  std::unique_ptr<RectificationBackend> rectifier_;
  std::unique_ptr<RectificationBackend> rgb_rectifier_;
  std::unique_ptr<NativeDisparityBackend> stereo_;
  std::vector<std::shared_ptr<FrameSetStorage>> frame_pool_;
  std::vector<std::uint8_t> disparity_validity_;
  std::vector<float> disparity_scratch_;
  std::vector<float> depth_scratch_;
  std::vector<std::uint8_t> confidence_scratch_;
  std::vector<std::uint8_t> depth_validity_scratch_;
  std::uint64_t stereo_generation_{};
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
  std::uint64_t protected_pending_dropped{};
  std::uint64_t processed{};
  std::uint64_t processing_failures{};
  std::uint64_t storage_exhausted_dropped{};
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
    StereoPair pair,
    ColorPreviewRequest color_preview = ColorPreviewRequest::kNone);
[[nodiscard]] bool cpu_dmabuf_mapping_available() noexcept;

// Copies only after consumer/pool admission. Mono is packed Gray8; color is
// packed full-range sYCC/Rec.601 I420 as pinned by native capture. RGB
// conversion writes directly into the caller-owned buffer.
void copy_capture_image(const StereoProcessingRequest& request, CameraRole role,
                        bool color, std::span<std::uint8_t> destination);
void copy_capture_rgb(const StereoProcessingRequest& request, CameraRole role,
                      std::span<std::uint8_t> destination);

struct CpuDmabufMappingStats {
  std::uint64_t mappings_started{};
  std::uint64_t sync_start_retries{};
  std::uint64_t sync_start_failures{};
  std::uint64_t sync_end_retries{};
  std::uint64_t sync_end_failures{};
  std::uint64_t mapping_cache_hits{};
  std::size_t cached_mappings{};
  std::size_t cached_bytes{};
};

// Process-lifetime counters make otherwise-noexcept DMA-BUF cleanup failures
// visible to status/diagnostic callers.
[[nodiscard]] CpuDmabufMappingStats cpu_dmabuf_mapping_stats() noexcept;

}  // namespace depthd
