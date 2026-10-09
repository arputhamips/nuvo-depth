#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "depthd/runtime.hpp"

namespace depthd {

// Raw consumer cadence is independent from preview work admitted on another
// capture. A preview-only request must not shift the next raw capture deadline.
class NativeCaptureCadence final {
 public:
  [[nodiscard]] bool admit(const FrameSetIdentity& identity,
                            ipc_v2::ProductMask products, double cadence_hz);
 private:
  std::optional<FrameSetIdentity> last_;
  ipc_v2::ProductMask products_{};
  double cadence_hz_{};
};

// Collects capture independently from processing and publication. The capture
// callbacks have one owner (the producer); publication consumes immutable
// frames through next(). The scheduler retains its sole stateful backend owner
// and replaces complete pending pairs under overload.
class NativeCaptureRunner final {
 public:
  // No request means capture was used only for status/metering/preview and
  // native image/depth products have no eligible subscriber this interval.
  using PrepareRequest =
      std::function<std::optional<StereoProcessingRequest>(StereoPair)>;
  using SubmissionObserver = std::function<void(
      std::shared_ptr<const CapturePublicationContext>, SubmitResult)>;

  NativeCaptureRunner(CaptureSource& capture, BoundedNativeScheduler& scheduler,
                      std::chrono::milliseconds pair_timeout,
                      std::function<void()> before_capture,
                      PrepareRequest prepare_request,
                      SubmissionObserver submission_observer = {});
  ~NativeCaptureRunner() noexcept;
  NativeCaptureRunner(const NativeCaptureRunner&) = delete;
  NativeCaptureRunner& operator=(const NativeCaptureRunner&) = delete;

  // A null result means no completion in this interval (including whole-pair
  // storage drops). Capture/backend failures are rethrown to the owner.
  [[nodiscard]] std::shared_ptr<const FrameSetStorage> next(
      std::chrono::milliseconds timeout);

  // Called by the publication owner. Joins capture collection, then drains
  // processing requests/mappings before stopping camera allocations. Idempotent.
  void stop() noexcept;

 private:
  void producer_loop() noexcept;
  void rethrow_failure();

  CaptureSource& capture_;
  BoundedNativeScheduler& scheduler_;
  std::chrono::milliseconds pair_timeout_;
  std::function<void()> before_capture_;
  PrepareRequest prepare_request_;
  SubmissionObserver submission_observer_;
  std::atomic<bool> stop_requested_{};
  std::mutex failure_mutex_;
  std::exception_ptr failure_;
  std::thread producer_;
  bool stopped_{};
};

}  // namespace depthd
