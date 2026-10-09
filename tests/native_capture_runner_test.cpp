#include "depthd/native_capture_runner.hpp"
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
#include "depthd/image_encoder.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

void require(const bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

template <typename Predicate>
void eventually(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  require(predicate(), message);
}

class FakeCapture final : public depthd::CaptureSource {
 public:
  std::vector<depthd::CameraIdentity> enumerate() override { return {}; }
  depthd::CaptureSessionInfo start(const depthd::CaptureOpenOptions&) override {
    return {};
  }
  void offer(const std::uint64_t sequence) {
    std::lock_guard lock(mutex_);
    newest_ = sequence;
    condition_.notify_one();
  }
  void fail() {
    std::lock_guard lock(mutex_);
    fail_ = true;
    condition_.notify_one();
  }
  std::optional<depthd::StereoPair> wait_for_pair(
      const std::chrono::milliseconds timeout) override {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, timeout, [&] { return newest_ != 0U || fail_; });
    if (fail_) throw std::runtime_error("fake capture failure");
    if (newest_ == 0U) return {};
    const auto sequence = std::exchange(newest_, 0U);
    lock.unlock();
    depthd::StereoPair pair;
    pair.capture_epoch = 3U;
    pair.pair_sequence = sequence;
    pair.capture_timestamp_ns = timestamp_base_ns +
        static_cast<std::int64_t>(sequence) * capture_period_ns;
    const auto fill = [&](depthd::CapturedFrame& frame,
                          const depthd::CameraRole role,
                          const std::uint64_t token) {
      frame.role = role;
      frame.camera_stable_id = role == depthd::CameraRole::kLogicalLeft
                                   ? "left-camera" : "right-camera";
      frame.capture_epoch = pair.capture_epoch;
      frame.stream_sequence = sequence;
      frame.sensor_timestamp_ns = pair.capture_timestamp_ns;
      frame.sensor_clock = depthd::CaptureClock::kBoottime;
      frame.grid = depthd::make_reference_grid(8U, 6U);
      frame.pixel_format = "YUV420";
      frame.isp_exposure_time_us = static_cast<std::int32_t>(sequence * 100U);
      ++outstanding_;
      maximum_outstanding_ = std::max(maximum_outstanding_, outstanding_.load());
      frame.lease = depthd::CaptureBufferLease(
          3U, token, [&] { --outstanding_; });
    };
    fill(pair.left, depthd::CameraRole::kLogicalLeft, sequence * 2U);
    fill(pair.right, depthd::CameraRole::kLogicalRight, sequence * 2U + 1U);
    ++collected_;
    return pair;
  }
  void set_controls(const depthd::CaptureControls&) override {}
  void stop() noexcept override {
    leases_at_stop_ = outstanding_.load();
    ++stop_calls_;
  }
  depthd::CaptureRuntimeStats runtime_stats() const noexcept override { return {}; }
  std::size_t outstanding_leases() const noexcept override {
    return outstanding_.load();
  }
  std::atomic<unsigned> collected_{};
  std::atomic<unsigned> stop_calls_{};
  unsigned leases_at_stop_{};
  unsigned maximum_outstanding_{};
  std::int64_t timestamp_base_ns{1'000'000};
  std::int64_t capture_period_ns{1};

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::uint64_t newest_{};
  bool fail_{};
  std::atomic<unsigned> outstanding_{};
};

struct TestContext final : depthd::CapturePublicationContext {
  std::uint64_t sequence{};
  std::int32_t reported_exposure{};
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
  depthd::encoding::CapturePlan encoding_plan;
#endif
};

depthd::StereoProcessingRequest prepare(depthd::StereoPair pair) {
  auto context = std::make_shared<TestContext>();
  context->sequence = pair.pair_sequence;
  context->reported_exposure = pair.left.isp_exposure_time_us;
  auto left = std::make_shared<std::vector<std::uint8_t>>(48U, 11U);
  auto right = std::make_shared<std::vector<std::uint8_t>>(48U, 22U);
  depthd::StereoProcessingRequest request;
  request.capture = std::move(pair);
  request.left_mapping_owner = left;
  request.right_mapping_owner = right;
  request.left = {left->data(), 8U, 6U, 8U, depthd::ImageEncoding::kGray8};
  request.right = {right->data(), 8U, 6U, 8U, depthd::ImageEncoding::kGray8};
  request.publication_context = std::move(context);
  return request;
}

class CopyRectifier final : public depthd::RectificationBackend {
 public:
  std::string_view name() const noexcept override { return "fake-copy"; }
  void rectify(depthd::RectificationCamera, const depthd::ImageView& input,
               const depthd::MutableImageView& output,
               depthd::RectificationStats&) override {
    for (std::uint32_t row = 0; row < input.height; ++row) {
      std::copy_n(input.data + row * input.stride_bytes, input.width,
                  output.data + row * output.stride_bytes);
    }
  }
};

struct Gate {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered{};
  bool released{};
  bool fail{};
  std::atomic<unsigned> calls{};
};

// An assertion failure must release the blocked backend before runner teardown
// joins it, so a regression produces a useful test failure instead of a hang.
struct ReleaseGate {
  std::shared_ptr<Gate> gate;
  ~ReleaseGate() {
    std::lock_guard lock(gate->mutex);
    gate->released = true;
    gate->condition.notify_all();
  }
};

class SlowStereo final : public depthd::NativeDisparityBackend {
 public:
  SlowStereo(const depthd::PixelGrid grid, std::shared_ptr<Gate> gate)
      : gate_(std::move(gate)) {
    metadata_.grid = grid;
    metadata_.valid_roi = {0U, 0U, grid.width, grid.height};
    metadata_.accepted_max_disparity_px = 32.0F;
    metadata_.algorithmic_quantization_px = 1.0F / 16.0F;
    metadata_.confidence_model = depthd::ConfidenceModel::kBinarySgbmSupportV1;
  }
  std::string_view name() const noexcept override { return "fake-slow-stereo"; }
  depthd::DisparityMetadata metadata() const noexcept override { return metadata_; }
  void process_native(const depthd::ImageView&, const depthd::ImageView&,
                      const depthd::MutableDisparityProductsView& output,
                      depthd::StereoBackendStats&) override {
    ++gate_->calls;
    {
      std::unique_lock lock(gate_->mutex);
      gate_->entered = true;
      gate_->condition.notify_all();
      gate_->condition.wait(lock, [&] { return gate_->released; });
      if (gate_->fail) throw std::runtime_error("fake backend failure");
    }
    for (std::uint32_t y = 0; y < metadata_.grid.height; ++y) {
      for (std::uint32_t x = 0; x < metadata_.grid.width; ++x) {
        const auto index = y * output.disparity.stride_elements + x;
        output.disparity.data[index] = 10.0F;
        output.confidence.data[index] = 255U;
        output.validity.data[index] = 0U;
      }
    }
  }

 private:
  depthd::DisparityMetadata metadata_;
  std::shared_ptr<Gate> gate_;
};

std::unique_ptr<depthd::NativeProcessingPipeline> pipeline(
    const std::shared_ptr<Gate>& gate, const std::size_t capacity = 4U) {
  depthd::NativePipelineConfiguration configuration;
  configuration.device_uuid[0] = 1U;
  configuration.run_uuid[0] = 2U;
  const auto grid = depthd::make_reference_grid(8U, 6U);
  const auto identity = depthd::calibration_identity_from_sha256(
      "1111111111111111111111111111111111111111111111111111111111111111");
  configuration.image_calibration = depthd::make_rectified_calibration(
      identity, depthd::CalibrationAuthority::kFactoryValidated, grid,
      {100.0, 0.0, 3.5, 0.0, 0.0, 100.0, 2.5, 0.0, 0.0, 0.0, 1.0, 0.0},
      {100.0, 0.0, 3.5, -10.0, 0.0, 100.0, 2.5, 0.0, 0.0, 0.0, 1.0, 0.0});
  configuration.matching_calibration = configuration.image_calibration;
  configuration.frame_pool_capacity = capacity;
  configuration.depth.minimum_depth_m = 0.01F;
  configuration.depth.maximum_depth_m = 10.0F;
  return std::make_unique<depthd::NativeProcessingPipeline>(
      configuration, std::make_unique<CopyRectifier>(),
      std::make_unique<SlowStereo>(grid, gate));
}

void check_context(const depthd::FrameSetStorage& frame) {
  const auto context = std::dynamic_pointer_cast<const TestContext>(
      frame.publication_context);
  require(context && context->sequence == frame.metadata.frame.capture_sequence,
          "publication context was associated with a different capture");
  require(context->reported_exposure ==
              static_cast<std::int32_t>(context->sequence * 100U),
          "published exposure metadata came from a newer capture");
}

void test_overlap_latest_and_finite_shutdown() {
  auto gate = std::make_shared<Gate>();
  FakeCapture capture;
  depthd::BoundedNativeScheduler scheduler({1U, 2U}, pipeline(gate));
  depthd::NativeCaptureRunner runner(capture, scheduler, 2s, {}, prepare);
  const ReleaseGate release_on_failure{gate};
  capture.offer(1U);
  {
    std::unique_lock lock(gate->mutex);
    require(gate->condition.wait_for(lock, 2s, [&] { return gate->entered; }),
            "slow backend did not begin processing");
  }
  for (std::uint64_t sequence = 2U; sequence <= 20U; ++sequence) {
    capture.offer(sequence);
    eventually([&] { return scheduler.stats().accepted >= sequence; },
               "processing/publication blocked new capture collection");
  }
  require(scheduler.stats().pending == 1U &&
              scheduler.stats().pending_replaced == 18U,
          "overload did not preserve one newest bounded pending pair");
  {
    std::lock_guard lock(gate->mutex);
    gate->released = true;
    gate->condition.notify_all();
  }
  // This models the daemon's finite published-frame limit. Collection remains
  // independent while exactly two coherent completions are consumed.
  for (unsigned published = 0; published < 2U; ++published) {
    const auto frame = runner.next(2s);
    require(frame != nullptr, "completed frame was lost at finite-frame limit");
    check_context(*frame);
    require(frame->metadata.frame.capture_sequence == (published == 0U ? 1U : 20U),
            "scheduler processed a stale pending capture under overload");
  }
  const auto stopped_at = std::chrono::steady_clock::now();
  runner.stop();
  runner.stop();
  require(std::chrono::steady_clock::now() - stopped_at < 1s,
          "shutdown waited for the complete pair timeout");
  require(capture.leases_at_stop_ == 0U && capture.outstanding_leases() == 0U &&
              capture.stop_calls_ == 1U && capture.maximum_outstanding_ <= 6U,
          "capture stopped before bounded pending/processing leases drained");
}


#if defined(DEPTHD_WITH_IMAGE_ENCODER)
void test_numeric_preview_with_independent_raw_cadence() {
  using depthd::ipc_v2::ProductKind;
  const auto mono = depthd::ipc_v2::product_bit(ProductKind::kLeftMono);
  const auto depth = depthd::ipc_v2::product_bit(ProductKind::kDepth);
  const auto output = std::filesystem::temp_directory_path() /
      "depthd-runner-numeric-demand-test" / "latest.nve";
  depthd::encoding::Configuration encoding;
  encoding.codec = depthd::encoding::Codec::kJpeg;
  encoding.source = depthd::encoding::Source::kLeftRgb;
  encoding.cadence_hz = 1U;
  encoding.width = 64U;
  encoding.height = 48U;
  const auto control = output.parent_path() / "encoding.json";
  std::filesystem::create_directories(output.parent_path());
  unsigned revision{};
  const auto write_control = [&] {
    { std::ofstream file(control); file << depthd::encoding::configuration_json(encoding); }
    std::filesystem::last_write_time(control,
        std::filesystem::file_time_type::clock::now() + std::chrono::seconds(++revision));
  };
  write_control();
  depthd::encoding::AsyncFrameEncoder encoder(control, output, encoding);
  auto gate = std::make_shared<Gate>();
  FakeCapture capture;
  capture.timestamp_base_ns = 1'000'000'000;
  capture.capture_period_ns = 16'666'667;
  depthd::BoundedNativeScheduler scheduler({1U, 2U}, pipeline(gate));
  depthd::NativeCaptureCadence raw_cadence;
  std::atomic<std::uint64_t> prepared{};
  std::atomic<bool> raw_demand{true};
  std::atomic<double> raw_cadence_hz{10.0};
  std::atomic<double> preview_cadence{1.0};
  depthd::NativeCaptureRunner runner(capture, scheduler, 2s, {},
      [&](depthd::StereoPair pair) -> std::optional<depthd::StereoProcessingRequest> {
        const auto identity = depthd::make_frame_set_identity(pair, {1U}, {2U}, 1U);
        auto products = raw_cadence.admit(identity, raw_demand ? mono : 0U, raw_cadence_hz)
            ? mono : 0U;
        encoder.set_demand(preview_cadence);
        depthd::encoding::CapturePlan plan;
        if (preview_cadence > 0.0) {
          if (depthd::encoding::requires_depth(encoder.configuration().source)) {
            plan = encoder.prepare_capture(identity);
          } else {
            depthd::ProductSetMetadata metadata;
            metadata.frame = identity;
            metadata.left_rectified.grid = depthd::make_reference_grid(8U, 6U);
            (void)encoder.submit_capture(metadata,
                [](depthd::CameraRole, bool, std::span<std::uint8_t> bytes) {
                  std::fill(bytes.begin(), bytes.end(), 128U);
                });
          }
        }
        const bool numeric_due = plan.result == depthd::encoding::SubmitResult::kAccepted;
        if (numeric_due) products |= depth;
        prepared = pair.pair_sequence;
        if (products == 0U) return std::nullopt;
        auto request = prepare(std::move(pair));
        request.products = products;
        request.preserve_for_preview = numeric_due;
        auto context = std::make_shared<TestContext>();
        context->sequence = identity.capture_sequence;
        context->encoding_plan = plan;
        request.publication_context = context;
        return request;
      }, [&](const std::shared_ptr<const depthd::CapturePublicationContext> publication,
              const depthd::SubmitResult result) {
        const auto context = std::dynamic_pointer_cast<const TestContext>(publication);
        if (result == depthd::SubmitResult::kAccepted && context &&
            context->encoding_plan.result == depthd::encoding::SubmitResult::kAccepted)
          (void)encoder.commit_native_capture(context->encoding_plan);
      });
  const ReleaseGate release_on_failure{gate};
  capture.offer(1U);
  auto raw_only = runner.next(2s);
  require(raw_only && raw_only->available_products == mono && gate->calls == 0U,
          "raw mono demand ran stereo/depth without a preview");
  eventually([&] { return encoder.stats().encoded == 1U; }, "initial RGB preview failed");
  capture.offer(2U);
  eventually([&] { return prepared >= 2U; }, "raw cadence stalled capture");
  // The daemon can publish this raw-only completion after the image source
  // changes to numeric. Its context deliberately has no initialized plan.
  encoding.source = depthd::encoding::Source::kDepth;
  write_control();
  encoder.refresh_configuration();
  preview_cadence = 1.0;
  const auto raw_context = std::dynamic_pointer_cast<const TestContext>(raw_only->publication_context);
  require(raw_context && raw_context->encoding_plan.identity.capture_sequence == 0U &&
              encoder.submit_native(raw_only, raw_context->encoding_plan) ==
                  depthd::encoding::SubmitResult::kDisabled && encoder.stats().failures == 0U,
          "RGB-to-numeric transition submitted a raw-only default plan");
  raw_only.reset();
  capture.offer(3U);  // Preview is due between the 10 Hz raw capture deadlines.
  eventually([&] { return gate->calls == 1U && encoder.stats().numeric_admitted == 1U; },
             "numeric preview did not submit independently of raw cadence");
  for (std::uint64_t sequence = 4U; sequence <= 131U; ++sequence) {
    capture.offer(sequence);
    eventually([&] { return prepared >= sequence; }, "slow matcher blocked numeric admission");
  }
  eventually([&] { return encoder.stats().numeric_admitted == 3U; },
             "numeric admission multiplied work at raw cadence");
  require(gate->calls == 1U && scheduler.stats().pending == 1U &&
              scheduler.stats().protected_pending_dropped >= 8U,
          "raw captures replaced a reserved pending numeric preview");
  {
    std::lock_guard lock(gate->mutex);
    gate->released = true;
    gate->condition.notify_all();
  }
  for (const auto sequence : {3U, 123U}) {
    const auto completed = runner.next(2s);
    require(completed && completed->metadata.frame.capture_sequence == sequence &&
                (completed->available_products & depth) != 0U,
            "numeric queue did not retain the newest protected coherent pair");
    const auto context = std::dynamic_pointer_cast<const TestContext>(completed->publication_context);
    require(context && context->encoding_plan.identity.capture_sequence == sequence &&
                encoder.submit_native(completed, context->encoding_plan) ==
                    depthd::encoding::SubmitResult::kAccepted,
            "reserved numeric preview could not encode its associated capture");
    const auto expected = sequence == 3U ? 2U : 3U;
    eventually([&] { return encoder.stats().encoded == expected; },
               "reserved numeric preview failed to publish");
  }
  require(gate->calls == 2U && encoder.stats().numeric_admitted == 3U,
          "slow numeric preview duplicated matcher work across 10 Hz raw frames");
  const auto accepted_before_idle = scheduler.stats().accepted;
  raw_demand = false;
  preview_cadence = 0.0;
  for (std::uint64_t sequence = 132U; sequence <= 140U; ++sequence) {
    capture.offer(sequence);
    eventually([&] { return prepared >= sequence; }, "idle capture stopped status collection");
  }
  require(gate->calls == 2U && scheduler.stats().accepted == accepted_before_idle &&
              encoder.stats().numeric_admitted == 3U,
          "idle demand ran unnecessary numeric stages");
  // The selected source stays numeric while encoded consumers disconnect.
  // New raw-only frames then carry default plans too, without stopping capture.
  raw_demand = true;
  capture.offer(141U);
  const auto disconnected_raw = runner.next(2s);
  const auto disconnected_context = disconnected_raw
      ? std::dynamic_pointer_cast<const TestContext>(disconnected_raw->publication_context) : nullptr;
  require(disconnected_raw && disconnected_raw->available_products == mono && disconnected_context &&
              disconnected_context->encoding_plan.identity.capture_sequence == 0U &&
              encoder.submit_native(disconnected_raw, disconnected_context->encoding_plan) ==
                  depthd::encoding::SubmitResult::kDisabled && gate->calls == 2U,
          "numeric preview disconnect tried to encode a raw-only default plan");

  encoding.source = depthd::encoding::Source::kLeftRgb;
  write_control();
  encoder.refresh_configuration();
  raw_cadence_hz = 1.0;
  preview_cadence = 1.0;
  capture.offer(142U);
  const auto slow_raw = runner.next(2s);
  require(slow_raw && slow_raw->available_products == mono, "new raw cadence was not admitted");
  const auto accepted_before_churn = scheduler.stats().accepted;
  for (std::uint64_t sequence = 143U; sequence <= 207U; ++sequence) {
    // Connecting, changing and disconnecting previews changes the global IPC
    // demand generation; raw deadlines must still depend only on raw demand.
    preview_cadence = static_cast<double>(sequence % 3U);
    capture.offer(sequence);
    eventually([&] { return prepared >= sequence; }, "preview churn blocked raw capture");
  }
  eventually([&] { return scheduler.stats().accepted >= accepted_before_churn + 1U; },
             "1 Hz raw demand lost its next deadline during preview churn");
  require(scheduler.stats().accepted == accepted_before_churn + 1U && gate->calls == 2U,
          "preview-only demand changes reset or accelerated raw processing cadence");
  runner.stop();
  encoder.stop();
  require(capture.outstanding_leases() == 0U, "protected numeric queue retained camera leases");
  std::filesystem::remove_all(output.parent_path());
}

void test_encoder_preflight_under_capture_overload() {
  const auto output = std::filesystem::temp_directory_path() /
      "depthd-runner-encoder-test" / "latest.nve";
  depthd::encoding::Configuration encoding;
  encoding.codec = depthd::encoding::Codec::kJpeg;
  encoding.source = depthd::encoding::Source::kLeftColor;
  encoding.width = 64U;
  encoding.height = 48U;
  depthd::encoding::AsyncFrameEncoder encoder({}, output, encoding);
  auto gate = std::make_shared<Gate>();
  FakeCapture capture;
  capture.timestamp_base_ns = 1'000'000'000;
  capture.capture_period_ns = 16'666'667;
  depthd::BoundedNativeScheduler scheduler({1U, 2U}, pipeline(gate));
  std::atomic<unsigned> optional_mappings{};
  std::atomic<unsigned> color_copies{};
  std::atomic<std::uint64_t> prepared{};
  std::atomic<bool> raw_demand{true};
  std::atomic<double> preview_demand_cadence{10.0};
  depthd::NativeCaptureRunner runner(capture, scheduler, 2s, {},
      [&](depthd::StereoPair pair) -> std::optional<depthd::StereoProcessingRequest> {
        depthd::ProductSetMetadata metadata;
        metadata.frame = depthd::make_frame_set_identity(pair, {1U}, {2U}, 1U);
        metadata.left_rectified.grid = depthd::make_reference_grid(8U, 6U);
        encoder.set_demand(preview_demand_cadence.load());
        (void)encoder.submit_capture(metadata,
            [&](depthd::CameraRole, bool, std::span<std::uint8_t> pixels) {
              ++optional_mappings;
              std::fill(pixels.begin(), pixels.end(), 128U);
              ++color_copies;
            });
        prepared = pair.pair_sequence;
        if (!raw_demand) return std::nullopt;
        return prepare(std::move(pair));
      });
  const ReleaseGate release_on_failure{gate};
  capture.offer(1U);
  eventually([&] {
    std::lock_guard lock(gate->mutex);
    return gate->entered;
  }, "slow stereo did not begin encoder overload test");
  eventually([&] { return encoder.stats().encoded == 1U; },
             "independent preview waited for the slow stereo matcher");
  for (std::uint64_t sequence = 2U; sequence <= 61U; ++sequence) {
    if (sequence == 32U) raw_demand = false;
    capture.offer(sequence);
    eventually([&] { return prepared >= sequence; },
               "preview branch blocked independent capture");
    if (sequence == 7U) {
      eventually([&] { return encoder.stats().encoded >= 2U; },
                 "next preview cadence waited for a depth completion");
      require(scheduler.stats().processed == 0U,
              "matcher gate did not model a blocked depth branch");
    }
  }
  require(optional_mappings == 11U && color_copies == 11U &&
              encoder.stats().image_admitted == 11U &&
              encoder.stats().capture_cadence_limited == 50U,
          "60 Hz capture performed more than the admitted 10 Hz preview copies");
  eventually([&] { return scheduler.stats().accepted == 31U; },
             "active raw subscription did not submit selected captures");
  preview_demand_cadence = 0.0;
  for (std::uint64_t sequence = 62U; sequence <= 70U; ++sequence) {
    capture.offer(sequence);
    eventually([&] { return prepared >= sequence; },
               "idle capture stopped status/metering collection");
  }
  require(scheduler.stats().accepted == 31U && optional_mappings == 11U &&
              encoder.stats().demand_cadence_hz == 0.0,
          "idle subscribers left native/preview work active");
  preview_demand_cadence = 2.0;
  for (std::uint64_t sequence = 71U; sequence <= 131U; ++sequence) {
    capture.offer(sequence);
    eventually([&] { return prepared >= sequence; },
               "re-enabled low-cadence preview blocked capture");
  }
  require(optional_mappings == 14U && color_copies == 14U &&
              encoder.stats().image_admitted == 14U,
          "2 Hz demand multiplied copies across 60 Hz captures");
  {
    std::lock_guard lock(gate->mutex);
    gate->released = true;
    gate->condition.notify_all();
  }
  runner.stop();
  encoder.stop();
  require(encoder.stats().failures == 0U && encoder.stats().image_pool_dropped == 0U,
          "independent preview admission failed under slow matcher load");
  require(capture.leases_at_stop_ == 0U,
          "independent preview retained camera leases through shutdown");
  std::filesystem::remove_all(output.parent_path());
}
#endif

void test_pinned_storage_is_a_drop() {
  auto gate = std::make_shared<Gate>();
  gate->released = true;
  FakeCapture capture;
  depthd::BoundedNativeScheduler scheduler({1U, 1U}, pipeline(gate, 1U));
  depthd::NativeCaptureRunner runner(capture, scheduler, 2s, {}, prepare);
  capture.offer(1U);
  auto pinned = runner.next(2s);
  require(pinned != nullptr, "initial pinned frame missing");
  for (std::uint64_t sequence = 2U; sequence <= 6U; ++sequence) {
    capture.offer(sequence);
    eventually([&] {
      return scheduler.stats().storage_exhausted_dropped >= sequence - 1U;
    }, "pinned pool did not drop an input whole");
  }
  require(!runner.next(20ms), "pinned pool fabricated an output");
  check_context(*pinned);
  pinned.reset();
  capture.offer(7U);
  const auto recovered = runner.next(2s);
  require(recovered && recovered->metadata.frame.capture_sequence == 7U,
          "processing failed to resume after consumer released pool storage");
  check_context(*recovered);
  runner.stop();
  require(capture.leases_at_stop_ == 0U, "pinned-drop shutdown retained leases");
}

void expect_failure(depthd::NativeCaptureRunner& runner, const char* text) {
  try {
    (void)runner.next(2s);
  } catch (const std::runtime_error& error) {
    require(std::string(error.what()).find(text) != std::string::npos,
            "wrong runner failure was surfaced");
    return;
  }
  throw std::runtime_error("runner swallowed a capture/backend failure");
}

void test_failures_and_stop_lease_order() {
  {
    auto gate = std::make_shared<Gate>();
    gate->released = true;
    FakeCapture capture;
    depthd::BoundedNativeScheduler scheduler({1U, 1U}, pipeline(gate));
    depthd::NativeCaptureRunner runner(capture, scheduler, 2s, {}, prepare);
    capture.fail();
    expect_failure(runner, "fake capture failure");
    runner.stop();
    require(capture.leases_at_stop_ == 0U, "capture failure retained leases");
  }
  {
    auto gate = std::make_shared<Gate>();
    gate->released = true;
    gate->fail = true;
    FakeCapture capture;
    depthd::BoundedNativeScheduler scheduler({1U, 1U}, pipeline(gate));
    depthd::NativeCaptureRunner runner(capture, scheduler, 2s, {}, prepare);
    capture.offer(1U);
    expect_failure(runner, "fake backend failure");
    runner.stop();
    require(capture.leases_at_stop_ == 0U, "backend failure retained leases");
  }
  {
    auto gate = std::make_shared<Gate>();
    gate->released = true;
    FakeCapture capture;
    depthd::BoundedNativeScheduler scheduler({1U, 1U}, pipeline(gate));
    depthd::NativeCaptureRunner runner(capture, scheduler, 80ms, {}, prepare);
    expect_failure(runner, "timed out waiting for a stereo pair");
    runner.stop();
  }
  {
    auto gate = std::make_shared<Gate>();
    gate->released = true;
    FakeCapture capture;
    depthd::BoundedNativeScheduler scheduler({1U, 1U}, pipeline(gate));
    depthd::NativeCaptureRunner runner(
        capture, scheduler, 2s, {}, [](depthd::StereoPair pair) {
          (void)pair;
          throw std::runtime_error("prepare callback failure");
          return depthd::StereoProcessingRequest{};
        });
    capture.offer(1U);
    expect_failure(runner, "prepare callback failure");
    runner.stop();
    require(capture.leases_at_stop_ == 0U, "preparation failure retained leases");
  }
}
}  // namespace

int main() {
  try {
    test_overlap_latest_and_finite_shutdown();
    test_pinned_storage_is_a_drop();
#if defined(DEPTHD_WITH_IMAGE_ENCODER)
    test_numeric_preview_with_independent_raw_cadence();
    test_encoder_preflight_under_capture_overload();
#endif
    test_failures_and_stop_lease_order();
    std::cout << "native capture runner tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
