#include "depthd/semantic_perception.hpp"

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace depthd {
namespace {

std::int64_t steady_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t realtime_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool cadence_due(const std::int64_t now_ns, const std::int64_t last_ns,
                 const std::int64_t period_ns) noexcept {
  if (last_ns == 0) {
    return true;
  }
  // Camera callbacks land a little either side of their nominal boundary.
  // A bounded 5 ms grace prevents a 5 Hz model on a 10 Hz camera from
  // accidentally falling to 3.3 Hz when the second frame is slightly early.
  const auto grace_ns = std::min<std::int64_t>(5'000'000LL, period_ns / 20);
  return now_ns - last_ns >= period_ns - grace_ns;
}

bool valid_frame(const SemanticFrame& frame) noexcept {
  if ((frame.left_rectified_bgr_data != nullptr ||
       frame.disparity_data != nullptr) &&
      frame.buffer_lease == nullptr) {
    return false;
  }
  const auto* color = frame.left_rectified_bgr_data != nullptr
      ? frame.left_rectified_bgr_data
      : (frame.left_rectified_bgr != nullptr
             ? frame.left_rectified_bgr->data()
             : nullptr);
  if (frame.source_sequence == 0U || frame.capture_realtime_ns <= 0 ||
      color == nullptr || frame.width == 0U ||
      frame.height == 0U || frame.color_stride_bytes < frame.width * 3U) {
    return false;
  }
  const auto required_color = frame.color_stride_bytes * frame.height;
  if (frame.left_rectified_bgr != nullptr &&
      frame.left_rectified_bgr->size() < required_color) {
    return false;
  }
  const auto* disparity = frame.disparity_data != nullptr
      ? frame.disparity_data
      : (frame.disparity != nullptr && !frame.disparity->empty()
             ? frame.disparity->data()
             : nullptr);
  if (disparity == nullptr) {
    return true;
  }
  return frame.disparity_stride_elements >= frame.width &&
         (frame.disparity == nullptr ||
          frame.disparity->size() >=
              frame.disparity_stride_elements * frame.height);
}

}  // namespace

class SemanticPerceptionModule::Impl {
 public:
  Impl(std::vector<SemanticBackendRegistration> registrations,
       SemanticPerceptionConfig config)
      : fusion_(config.fusion),
        preview_encoder_(std::move(config.preview_encoder)),
        callback_(std::move(config.result_callback)) {
    if (registrations.empty()) {
      throw std::invalid_argument(
          "semantic perception requires at least one backend");
    }
    backends_.reserve(registrations.size());
    for (auto& registration : registrations) {
      if (registration.backend == nullptr ||
          !std::isfinite(registration.cadence_hz) ||
          registration.cadence_hz <= 0.0) {
        throw std::invalid_argument("invalid semantic backend registration");
      }
      BackendState state;
      state.info = registration.backend->info();
      if (state.info.model_name.empty()) {
        throw std::invalid_argument("semantic backend model name is empty");
      }
      state.period_ns = static_cast<std::int64_t>(
          std::llround(1'000'000'000.0 / registration.cadence_hz));
      state.stats.model_name = state.info.model_name;
      state.backend = std::move(registration.backend);
      backends_.push_back(std::move(state));
    }
    worker_ = std::thread([this] { run(); });
  }

  ~Impl() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      pending_.reset();
    }
    condition_.notify_all();
    result_condition_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  bool submit(SemanticFrame frame) noexcept {
    if (!valid_frame(frame)) {
      return false;
    }
    if (frame.submitted_steady_ns <= 0) {
      frame.submitted_steady_ns = steady_now_ns();
    }
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        return false;
      }
      ++stats_.submitted_frames;
      if (pending_.has_value()) {
        ++stats_.replaced_pending_frames;
      }
      pending_ = std::move(frame);
    }
    condition_.notify_one();
    return true;
  }

  bool wants_frame(const std::int64_t schedule_steady_ns) const noexcept {
    if (schedule_steady_ns <= 0) {
      return false;
    }
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return false;
    }
    return std::any_of(backends_.begin(), backends_.end(),
                       [schedule_steady_ns](const BackendState& backend) {
      return cadence_due(schedule_steady_ns, backend.last_run_steady_ns,
                         backend.period_ns);
    });
  }

  SemanticPerceptionStats stats() const {
    std::lock_guard lock(mutex_);
    auto output = stats_;
    output.pending_frame = pending_.has_value();
    output.backends.reserve(backends_.size());
    for (const auto& backend : backends_) {
      output.backends.push_back(backend.stats);
    }
    return output;
  }

  std::optional<SemanticFrameResult> latest_result() const {
    std::lock_guard lock(mutex_);
    return latest_result_;
  }

  bool wait_for_result_after(const std::uint64_t source_sequence,
                             const std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return result_condition_.wait_for(lock, timeout, [this, source_sequence] {
      return stopping_ || (latest_result_.has_value() &&
          latest_result_->source_sequence > source_sequence);
    });
  }

 private:
  struct BackendState {
    std::unique_ptr<ISemanticBackend> backend{};
    SemanticBackendInfo info{};
    std::int64_t period_ns{};
    std::int64_t last_run_steady_ns{};
    SemanticBackendStats stats{};
  };

  void run() noexcept {
    while (true) {
      SemanticFrame frame;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
        if (stopping_) {
          return;
        }
        frame = std::move(*pending_);
        pending_.reset();
      }
      process(frame);
    }
  }

  void process(const SemanticFrame& frame) noexcept {
    const auto frame_started_ns = steady_now_ns();
    const auto schedule_ns = frame.submitted_steady_ns;
    std::vector<SemanticObservation> observations;
    std::vector<SemanticModelRun> model_runs;
    std::uint32_t successful_runs = 0U;
    std::uint32_t failed_runs = 0U;

    const auto* color = frame.left_rectified_bgr_data != nullptr
        ? frame.left_rectified_bgr_data
        : frame.left_rectified_bgr->data();
    const SemanticImageView image{color, frame.width, frame.height,
                                  frame.color_stride_bytes};
    for (auto& backend : backends_) {
      bool due = false;
      {
        std::lock_guard lock(mutex_);
        due = cadence_due(schedule_ns, backend.last_run_steady_ns,
                          backend.period_ns);
        if (due) {
          backend.last_run_steady_ns = schedule_ns;
        }
      }
      if (!due) {
        continue;
      }
      const auto started_ns = steady_now_ns();
      SemanticModelRun run;
      run.model_name = backend.info.model_name;
      run.model_version = backend.info.model_version;
      run.model_sha256 = backend.info.model_sha256;
      try {
        auto output = backend.backend->infer(image);
        std::vector<SemanticObservation> fresh;
        fresh.reserve(output.instances.size() + output.dense_regions.size());
        for (auto& instance : output.instances) {
          instance.source_kind = SemanticSourceKind::kInstance;
          if (instance.model_name.empty()) {
            instance.model_name = backend.info.model_name;
          }
          fresh.push_back(std::move(instance));
        }
        for (auto& region : output.dense_regions) {
          region.source_kind = SemanticSourceKind::kDenseRegion;
          if (region.model_name.empty()) {
            region.model_name = backend.info.model_name;
          }
          fresh.push_back(std::move(region));
        }
        observations.insert(observations.end(),
                            std::make_move_iterator(fresh.begin()),
                            std::make_move_iterator(fresh.end()));
        run.health = SemanticHealthState::kOk;
        ++successful_runs;
        {
          std::lock_guard lock(mutex_);
          ++backend.stats.completed_runs;
          run.completed_runs = backend.stats.completed_runs;
          run.errors = backend.stats.errors;
        }
      } catch (...) {
        run.health = SemanticHealthState::kFault;
        ++failed_runs;
        {
          std::lock_guard lock(mutex_);
          ++backend.stats.errors;
          run.completed_runs = backend.stats.completed_runs;
          run.errors = backend.stats.errors;
          ++stats_.worker_errors;
        }
      }
      run.latency_ms = static_cast<double>(steady_now_ns() - started_ns) / 1e6;
      const auto timing = backend.backend->last_timing();
      run.inference_latency_ms = timing.inference_latency_ms > 0.0
          ? timing.inference_latency_ms : run.latency_ms;
      run.postprocess_latency_ms = timing.postprocess_latency_ms;
      {
        std::lock_guard lock(mutex_);
        backend.stats.last_latency_ms = run.latency_ms;
        backend.stats.max_latency_ms =
            std::max(backend.stats.max_latency_ms, run.latency_ms);
      }
      model_runs.push_back(std::move(run));
    }

    {
      std::lock_guard lock(mutex_);
      ++stats_.processed_frames;
    }
    if (model_runs.empty()) {
      return;
    }

    SemanticFrameResult result;
    result.source_sequence = frame.source_sequence;
    result.capture_realtime_ns = frame.capture_realtime_ns;
    result.ready_realtime_ns = realtime_now_ns();
    result.models = std::move(model_runs);
    result.health = failed_runs == 0U
        ? SemanticHealthState::kOk
        : (successful_runs == 0U ? SemanticHealthState::kFault
                                 : SemanticHealthState::kDegraded);

    DisparityFrameView disparity;
    const auto* disparity_data = frame.disparity_data != nullptr
        ? frame.disparity_data
        : (frame.disparity != nullptr && !frame.disparity->empty()
               ? frame.disparity->data()
               : nullptr);
    if (disparity_data != nullptr) {
      disparity.data = disparity_data;
      disparity.width = frame.width;
      disparity.height = frame.height;
      disparity.stride_elements = frame.disparity_stride_elements;
      disparity.valid_x_offset = frame.valid_window_x;
      disparity.valid_y_offset = frame.valid_window_y;
      disparity.valid_width = frame.valid_window_width;
      disparity.valid_height = frame.valid_window_height;
      disparity.min_disparity = frame.min_disparity;
      disparity.max_disparity = frame.max_disparity;
    }
    result.objects = fusion_.fuse_all(
        observations, disparity, frame.camera, frame.optical_to_target);
    result.hazard = fusion_.evaluate_hazard(
        result.objects, result.health != SemanticHealthState::kOk);
    if (preview_encoder_) {
      try {
        result.preview_jpeg = preview_encoder_(image);
      } catch (...) {
        std::lock_guard lock(mutex_);
        ++stats_.worker_errors;
      }
    }

    const auto frame_latency_ms =
        static_cast<double>(steady_now_ns() - frame_started_ns) / 1e6;
    SemanticPerceptionStats callback_stats;
    {
      std::lock_guard lock(mutex_);
      latest_result_ = result;
      ++stats_.published_results;
      stats_.last_frame_latency_ms = frame_latency_ms;
      stats_.max_frame_latency_ms =
          std::max(stats_.max_frame_latency_ms, frame_latency_ms);
      callback_stats = stats_;
      callback_stats.pending_frame = pending_.has_value();
      callback_stats.backends.reserve(backends_.size());
      for (const auto& backend : backends_) {
        callback_stats.backends.push_back(backend.stats);
      }
    }
    result_condition_.notify_all();
    if (callback_) {
      try {
        callback_(result, callback_stats);
      } catch (...) {
        std::lock_guard lock(mutex_);
        ++stats_.worker_errors;
      }
    }
  }

  SemanticDepthFusion fusion_;
  std::function<std::shared_ptr<const std::vector<std::uint8_t>>(
      const SemanticImageView&)> preview_encoder_;
  std::function<void(const SemanticFrameResult&,
                     const SemanticPerceptionStats&)> callback_;
  std::vector<BackendState> backends_{};
  std::thread worker_{};
  mutable std::mutex mutex_{};
  std::condition_variable condition_{};
  mutable std::condition_variable result_condition_{};
  bool stopping_{};
  std::optional<SemanticFrame> pending_{};
  std::optional<SemanticFrameResult> latest_result_{};
  SemanticPerceptionStats stats_{};
};

SemanticPerceptionModule::SemanticPerceptionModule(
    std::vector<SemanticBackendRegistration> backends,
    SemanticPerceptionConfig config)
    : impl_(std::make_unique<Impl>(std::move(backends), std::move(config))) {}

SemanticPerceptionModule::~SemanticPerceptionModule() = default;

bool SemanticPerceptionModule::submit(SemanticFrame frame) noexcept {
  return impl_->submit(std::move(frame));
}

bool SemanticPerceptionModule::wants_frame(
    const std::int64_t schedule_steady_ns) const noexcept {
  return impl_->wants_frame(schedule_steady_ns);
}

SemanticPerceptionStats SemanticPerceptionModule::stats() const {
  return impl_->stats();
}

std::optional<SemanticFrameResult> SemanticPerceptionModule::latest_result()
    const {
  return impl_->latest_result();
}

bool SemanticPerceptionModule::wait_for_result_after(
    const std::uint64_t source_sequence,
    const std::chrono::milliseconds timeout) const {
  return impl_->wait_for_result_after(source_sequence, timeout);
}

}  // namespace depthd
