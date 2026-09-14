#include "depthd/capture.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace depthd {
namespace {

void validate_config(const PairerConfig& config) {
  if (config.pending_capacity_per_camera == 0U) {
    throw std::invalid_argument("pairer pending capacity must be non-zero");
  }
  if (config.tolerance_ns >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("pairer tolerance exceeds timestamp range");
  }
}

std::size_t role_index(const CameraRole role) {
  switch (role) {
    case CameraRole::kLogicalLeft:
      return 0U;
    case CameraRole::kLogicalRight:
      return 1U;
  }
  throw std::invalid_argument("captured frame has an unknown camera role");
}

std::uint64_t timestamp_delta(const std::int64_t left,
                              const std::int64_t right) {
  return left >= right ? static_cast<std::uint64_t>(left - right)
                       : static_cast<std::uint64_t>(right - left);
}

std::int64_t timestamp_midpoint(const std::int64_t left,
                                const std::int64_t right) {
  return left <= right ? left + (right - left) / 2
                       : right + (left - right) / 2;
}

}  // namespace

StereoPairer::StereoPairer(const PairerConfig config) : config_(config) {
  validate_config(config_);
}

StereoPairer::~StereoPairer() { end_epoch(); }

void StereoPairer::configure(const PairerConfig config) {
  validate_config(config);
  std::lock_guard lock(mutex_);
  if (running_) {
    throw std::logic_error("cannot configure pairer during a capture epoch");
  }
  config_ = config;
}

std::uint64_t StereoPairer::begin_epoch(const CameraBinding& left,
                                        const CameraBinding& right) {
  validate_stereo_bindings(left, right);
  std::lock_guard lock(mutex_);
  if (running_) {
    throw std::logic_error("capture epoch is already active");
  }
  if (bindings_pinned_ &&
      (left_binding_.camera.stable_id != left.camera.stable_id ||
       right_binding_.camera.stable_id != right.camera.stable_id)) {
    throw std::invalid_argument(
        "logical camera binding changed after it was pinned");
  }
  if (epoch_counter_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("capture epoch counter exhausted");
  }
  if (!bindings_pinned_) {
    left_binding_ = left;
    right_binding_ = right;
    bindings_pinned_ = true;
  }
  ++epoch_counter_;
  pair_sequence_ = 0U;
  last_stream_sequence_ = {};
  last_timestamp_ns_ = {};
  left_pending_.clear();
  right_pending_.clear();
  stats_ = {};
  running_ = true;
  return epoch_counter_;
}

void StereoPairer::end_epoch() noexcept {
  std::deque<CapturedFrame> left;
  std::deque<CapturedFrame> right;
  {
    std::lock_guard lock(mutex_);
    running_ = false;
    left.swap(left_pending_);
    right.swap(right_pending_);
  }
}

std::optional<StereoPair> StereoPairer::submit(CapturedFrame frame) {
  std::vector<CapturedFrame> releases;
  std::optional<StereoPair> result;
  {
    std::lock_guard lock(mutex_);
    if (!running_) {
      ++stats_.epoch_rejected;
      return std::nullopt;
    }

    const std::size_t index = role_index(frame.role);
    if (index == 0U) {
      ++stats_.left_submitted;
    } else {
      ++stats_.right_submitted;
    }

    const auto& expected = index == 0U ? left_binding_ : right_binding_;
    if (frame.camera_stable_id != expected.camera.stable_id) {
      ++stats_.identity_rejected;
      return std::nullopt;
    }
    if (frame.capture_epoch != epoch_counter_) {
      ++stats_.epoch_rejected;
      return std::nullopt;
    }
    if (frame.sensor_timestamp_ns <= 0) {
      ++stats_.zero_timestamp_rejected;
      return std::nullopt;
    }
    if (frame.sensor_clock != CaptureClock::kBoottime) {
      ++stats_.wrong_clock_rejected;
      return std::nullopt;
    }
    if (frame.stream_sequence == 0U) {
      ++stats_.nonmonotonic_rejected;
      return std::nullopt;
    }
    if (frame.stream_sequence == last_stream_sequence_[index]) {
      ++stats_.duplicate_sequence_rejected;
      return std::nullopt;
    }
    if (frame.stream_sequence < last_stream_sequence_[index] ||
        frame.sensor_timestamp_ns <= last_timestamp_ns_[index]) {
      ++stats_.nonmonotonic_rejected;
      return std::nullopt;
    }

    validate(frame.grid);
    if (frame.pixel_format.empty()) {
      throw std::invalid_argument("captured frame pixel format is empty");
    }
    if (frame.planes.empty()) {
      throw std::invalid_argument("captured frame has no buffer planes");
    }
    for (const auto& plane : frame.planes) {
      validate(plane);
    }
    if (!frame.lease.active() ||
        frame.lease.capture_epoch() != frame.capture_epoch) {
      throw std::invalid_argument(
          "captured frame has no active lease for its epoch");
    }

    last_stream_sequence_[index] = frame.stream_sequence;
    last_timestamp_ns_[index] = frame.sensor_timestamp_ns;
    auto& pending = index == 0U ? left_pending_ : right_pending_;
    pending.push_back(std::move(frame));
    if (pending.size() > config_.pending_capacity_per_camera) {
      releases.push_back(std::move(pending.front()));
      pending.pop_front();
      ++stats_.capacity_dropped;
    }

    while (!left_pending_.empty() && !right_pending_.empty()) {
      const auto left_time = left_pending_.front().sensor_timestamp_ns;
      const auto right_time = right_pending_.front().sensor_timestamp_ns;
      const auto delta = timestamp_delta(left_time, right_time);
      if (delta <= config_.tolerance_ns) {
        if (pair_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
          throw std::overflow_error("capture pair sequence exhausted");
        }
        StereoPair pair;
        pair.left = std::move(left_pending_.front());
        left_pending_.pop_front();
        pair.right = std::move(right_pending_.front());
        right_pending_.pop_front();
        pair.capture_epoch = epoch_counter_;
        pair.pair_sequence = ++pair_sequence_;
        pair.capture_timestamp_ns = timestamp_midpoint(left_time, right_time);
        pair.sensor_delta_ns = delta;
        ++stats_.paired;
        stats_.maximum_sensor_delta_ns =
            std::max(stats_.maximum_sensor_delta_ns, delta);
        result.emplace(std::move(pair));
        break;
      }

      auto& stale = left_time < right_time ? left_pending_ : right_pending_;
      releases.push_back(std::move(stale.front()));
      stale.pop_front();
      ++stats_.stale_dropped;
    }
  }
  return result;
}

PairerStats StereoPairer::stats() const noexcept {
  std::lock_guard lock(mutex_);
  return stats_;
}

std::uint64_t StereoPairer::active_epoch() const noexcept {
  std::lock_guard lock(mutex_);
  return running_ ? epoch_counter_ : 0U;
}

bool StereoPairer::running() const noexcept {
  std::lock_guard lock(mutex_);
  return running_;
}

}  // namespace depthd
