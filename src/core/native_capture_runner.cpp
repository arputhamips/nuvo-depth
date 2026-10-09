#include "depthd/native_capture_runner.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace depthd {

namespace {
constexpr auto kCaptureWaitSlice = std::chrono::milliseconds(50);
}

bool NativeCaptureCadence::admit(const FrameSetIdentity& identity,
                                 const ipc_v2::ProductMask products,
                                 const double cadence_hz) {
  if (!std::isfinite(cadence_hz) || cadence_hz < 0.0 || cadence_hz > 60.0) {
    throw std::invalid_argument("raw capture cadence must be between 0 and 60 Hz");
  }
  // IPC's global demand generation also changes on preview-only churn. Raw
  // deadlines depend only on the raw request and capture scope.
  if (products_ != products || cadence_hz_ != cadence_hz ||
      (last_ && (last_->device_uuid != identity.device_uuid ||
                 last_->run_uuid != identity.run_uuid ||
                 last_->capture_epoch != identity.capture_epoch ||
                 last_->configuration_epoch != identity.configuration_epoch))) {
    last_.reset();
    products_ = products;
    cadence_hz_ = cadence_hz;
  }
  if (products == 0U || cadence_hz == 0.0) return false;
  if (last_ && identity.capture_time_ns > last_->capture_time_ns) {
    const auto period = static_cast<std::int64_t>(1'000'000'000.0 / cadence_hz);
    const auto tolerance = std::min<std::int64_t>(period / 10, 2'000'000);
    if (identity.capture_time_ns - last_->capture_time_ns + tolerance < period) return false;
  }
  last_ = identity;
  return true;
}

NativeCaptureRunner::NativeCaptureRunner(
    CaptureSource& capture, BoundedNativeScheduler& scheduler,
    const std::chrono::milliseconds pair_timeout,
    std::function<void()> before_capture, PrepareRequest prepare_request,
    SubmissionObserver submission_observer)
    : capture_(capture),
      scheduler_(scheduler),
      pair_timeout_(pair_timeout),
      before_capture_(std::move(before_capture)),
      prepare_request_(std::move(prepare_request)),
      submission_observer_(std::move(submission_observer)) {
  if (pair_timeout_.count() <= 0 || !prepare_request_) {
    throw std::invalid_argument("capture runner requires a timeout and factory");
  }
  producer_ = std::thread(&NativeCaptureRunner::producer_loop, this);
}

NativeCaptureRunner::~NativeCaptureRunner() noexcept { stop(); }

void NativeCaptureRunner::producer_loop() noexcept {
  try {
    while (!stop_requested_.load()) {
      if (before_capture_) {
        before_capture_();
      }
      const auto deadline = std::chrono::steady_clock::now() + pair_timeout_;
      std::optional<StereoPair> pair;
      while (!stop_requested_.load() && !pair) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          throw std::runtime_error("timed out waiting for a stereo pair");
        }
        pair = capture_.wait_for_pair(std::min(kCaptureWaitSlice, remaining));
      }
      if (stop_requested_.load()) {
        break;
      }
      auto request = prepare_request_(std::move(*pair));
      if (!request) {
        continue;
      }
      if (stop_requested_.load()) {
        break;
      }
      const auto context = request->publication_context;
      const auto result = scheduler_.try_submit(std::move(*request));
      if (submission_observer_) submission_observer_(context, result);
      if (result == SubmitResult::kStopped && !stop_requested_.load()) {
        throw std::runtime_error("native scheduler rejected a capture pair");
      }
    }
  } catch (...) {
    std::lock_guard lock(failure_mutex_);
    failure_ = std::current_exception();
  }
}

void NativeCaptureRunner::rethrow_failure() {
  std::exception_ptr failure;
  {
    std::lock_guard lock(failure_mutex_);
    failure = failure_;
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
  if (const auto detail = scheduler_.take_last_error(); !detail.empty()) {
    throw std::runtime_error("native processing failed: " + detail);
  }
}

std::shared_ptr<const FrameSetStorage> NativeCaptureRunner::next(
    const std::chrono::milliseconds timeout) {
  if (timeout.count() < 0) {
    throw std::invalid_argument("capture runner wait cannot be negative");
  }
  rethrow_failure();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    const auto remaining = std::max(
        std::chrono::milliseconds(0),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()));
    auto frame = scheduler_.wait_next(std::min(kCaptureWaitSlice, remaining));
    rethrow_failure();
    if (frame) {
      return frame;
    }
  } while (!stopped_ && std::chrono::steady_clock::now() < deadline);
  return {};
}

void NativeCaptureRunner::stop() noexcept {
  if (stopped_) {
    return;
  }
  stop_requested_.store(true);
  if (producer_.joinable()) {
    producer_.join();
  }
  scheduler_.stop();
  capture_.stop();
  stopped_ = true;
}

}  // namespace depthd
