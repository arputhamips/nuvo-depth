#include "depthd/native_semantic_runtime_c.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

#include "depthd/hailort_semantic_backend.hpp"
#include "depthd/native_output.hpp"
#include "depthd/semantic_perception.hpp"
#include "depthd/semantic_ring.hpp"

#if defined(DEPTHD_WITH_HAILORT_SEMANTICS)
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#endif

namespace {

void set_error(char* destination, const std::size_t capacity,
               const std::string& message) noexcept {
  if (destination == nullptr || capacity == 0U) {
    return;
  }
  const auto size = std::min(capacity - 1U, message.size());
  std::memcpy(destination, message.data(), size);
  destination[size] = '\0';
}

void clear_error(char* destination, const std::size_t capacity) noexcept {
  if (destination != nullptr && capacity > 0U) {
    destination[0] = '\0';
  }
}

#if defined(DEPTHD_WITH_HAILORT_SEMANTICS)
std::string required_string(const char* value, const char* name) {
  if (value == nullptr || value[0] == '\0') {
    throw std::invalid_argument(std::string(name) + " is empty");
  }
  return value;
}
#endif

struct FrameLease {
  depthd_native_semantic_release_fn release{};
  void* context{};
  std::uint64_t token{};

  ~FrameLease() {
    if (release != nullptr) {
      release(context, token);
    }
  }
};

struct RuntimeShared {
  explicit RuntimeShared(depthd::SemanticRingConfig config)
      : ring(std::move(config)) {}

  depthd::SemanticRingWriter ring;
};

}  // namespace

struct depthd_native_semantic_handle {
  depthd_native_semantic_release_fn release{};
  void* release_context{};
  std::shared_ptr<RuntimeShared> shared{};
  depthd::RigidTransform optical_to_target{};
  std::int64_t submit_period_ns{};
  mutable std::int64_t last_reserved_steady_ns{};
  mutable std::mutex schedule_mutex{};
  std::unique_ptr<depthd::SemanticPerceptionModule> module{};
};

extern "C" {

void depthd_native_semantic_default_config(
    depthd_native_semantic_config* config) {
  if (config == nullptr) {
    return;
  }
  std::memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->yolo_hz = 5.0;
  config->segformer_hz = 5.0;
  config->preview_jpeg_quality = 80U;
  const auto transform = depthd::make_nuvo_left_optical_to_base_transform();
  std::copy(transform.matrix.begin(), transform.matrix.end(),
            config->optical_to_target);
}

int depthd_native_semantic_hailort_available(void) {
#if defined(DEPTHD_WITH_HAILORT_SEMANTICS)
  return 1;
#else
  return 0;
#endif
}

depthd_native_semantic_handle* depthd_native_semantic_create(
    const depthd_native_semantic_config* config, char* error,
    const std::size_t error_capacity) {
  clear_error(error, error_capacity);
  try {
    if (config == nullptr || config->struct_size != sizeof(*config)) {
      throw std::invalid_argument("native semantic config ABI size mismatch");
    }
    if (config->release_frame == nullptr) {
      throw std::invalid_argument("native semantic release callback is null");
    }
#if !defined(DEPTHD_WITH_HAILORT_SEMANTICS)
    throw std::runtime_error(
        "native semantic runtime was built without HailoRT support");
#else
    if (!std::isfinite(config->yolo_hz) || config->yolo_hz <= 0.0 ||
        !std::isfinite(config->segformer_hz) ||
        config->segformer_hz <= 0.0) {
      throw std::invalid_argument("native semantic cadence must be positive");
    }
    if (config->preview_jpeg_quality < 1U ||
        config->preview_jpeg_quality > 100U) {
      throw std::invalid_argument("semantic preview JPEG quality is invalid");
    }

    depthd::SemanticRingConfig ring;
    if (config->semantic_ring_path != nullptr &&
        config->semantic_ring_path[0] != '\0') {
      ring.path = config->semantic_ring_path;
    }
    if (config->target_frame != nullptr && config->target_frame[0] != '\0') {
      ring.target_frame = config->target_frame;
    }
    auto handle = std::make_unique<depthd_native_semantic_handle>();
    handle->release = config->release_frame;
    handle->release_context = config->release_context;
    handle->submit_period_ns = static_cast<std::int64_t>(std::llround(
        1'000'000'000.0 / std::max(config->yolo_hz, config->segformer_hz)));
    handle->shared = std::make_shared<RuntimeShared>(std::move(ring));

    depthd::HailortSemanticModelConfig models;
    models.yolo_hef_path = required_string(config->yolo_hef_path, "YOLO HEF");
    models.segformer_hef_path = required_string(
        config->segformer_hef_path, "SegFormer HEF");
    models.yolo_hz = config->yolo_hz;
    models.segformer_hz = config->segformer_hz;

    depthd::SemanticPerceptionConfig perception;
    std::copy(std::begin(config->optical_to_target),
              std::end(config->optical_to_target),
              handle->optical_to_target.matrix.begin());
    const auto shared = handle->shared;
    perception.result_callback =
        [shared](const depthd::SemanticFrameResult& result,
                 const depthd::SemanticPerceptionStats& stats) {
          const auto preview = result.preview_jpeg != nullptr
              ? std::span<const std::uint8_t>(*result.preview_jpeg)
              : std::span<const std::uint8_t>{};
          (void)shared->ring.publish({&result, &stats, preview});
        };
    if (config->preview_enabled != 0U) {
      const auto quality = static_cast<int>(config->preview_jpeg_quality);
      perception.preview_encoder =
          [quality](const depthd::SemanticImageView& image) {
            const cv::Mat source(
                static_cast<int>(image.height), static_cast<int>(image.width),
                CV_8UC3, const_cast<std::uint8_t*>(image.bgr),
                image.stride_bytes);
            auto encoded =
                std::make_shared<std::vector<std::uint8_t>>();
            if (!cv::imencode(".jpg", source, *encoded,
                              {cv::IMWRITE_JPEG_QUALITY, quality})) {
              throw std::runtime_error("semantic preview JPEG encode failed");
            }
            return std::shared_ptr<const std::vector<std::uint8_t>>(
                std::move(encoded));
          };
    }
    handle->module = std::make_unique<depthd::SemanticPerceptionModule>(
        depthd::make_hailort_semantic_backends(models),
        std::move(perception));
    return handle.release();
#endif
  } catch (const std::exception& exception) {
    set_error(error, error_capacity, exception.what());
    return nullptr;
  }
}

int depthd_native_semantic_wants_frame(
    const depthd_native_semantic_handle* handle,
    const std::int64_t submitted_steady_ns) {
  if (handle == nullptr || handle->module == nullptr) {
    return 0;
  }
  std::lock_guard lock(handle->schedule_mutex);
  const auto grace_ns = std::min<std::int64_t>(
      5'000'000LL, handle->submit_period_ns / 20);
  if (handle->last_reserved_steady_ns != 0 &&
      submitted_steady_ns - handle->last_reserved_steady_ns <
          handle->submit_period_ns - grace_ns) {
    return 0;
  }
  if (!handle->module->wants_frame(submitted_steady_ns)) {
    return 0;
  }
  handle->last_reserved_steady_ns = submitted_steady_ns;
  return 1;
}

int depthd_native_semantic_submit(depthd_native_semantic_handle* handle,
                                  const depthd_native_semantic_frame* frame,
                                  char* error,
                                  const std::size_t error_capacity) {
  clear_error(error, error_capacity);
  if (handle == nullptr || handle->module == nullptr) {
    set_error(error, error_capacity, "native semantic handle is null");
    return 0;
  }
  auto lease = std::make_shared<FrameLease>(
      FrameLease{handle->release, handle->release_context,
                 frame != nullptr ? frame->lease_token : 0U});
  try {
    if (frame == nullptr || frame->struct_size != sizeof(*frame)) {
      throw std::invalid_argument("native semantic frame ABI size mismatch");
    }
    if (frame->lease_token == 0U || frame->source_sequence == 0U ||
        frame->capture_realtime_ns <= 0 ||
        frame->submitted_steady_ns <= 0 ||
        frame->left_rectified_bgr == nullptr || frame->disparity == nullptr ||
        frame->width != 728U || frame->height != 544U ||
        frame->color_stride_bytes < frame->width * 3U ||
        frame->disparity_stride_elements < frame->width) {
      throw std::invalid_argument("native semantic frame is invalid");
    }
    depthd::SemanticFrame native;
    native.source_sequence = frame->source_sequence;
    native.capture_realtime_ns = frame->capture_realtime_ns;
    native.submitted_steady_ns = frame->submitted_steady_ns;
    native.buffer_lease = std::move(lease);
    native.left_rectified_bgr_data = frame->left_rectified_bgr;
    native.width = frame->width;
    native.height = frame->height;
    native.color_stride_bytes = frame->color_stride_bytes;
    native.disparity_data = frame->disparity;
    native.disparity_stride_elements = frame->disparity_stride_elements;
    native.valid_window_x = frame->valid_x_offset;
    native.valid_window_y = frame->valid_y_offset;
    native.valid_window_width = frame->valid_width;
    native.valid_window_height = frame->valid_height;
    native.min_disparity = frame->min_disparity;
    native.max_disparity = frame->max_disparity;
    native.optical_to_target = handle->optical_to_target;
    if (!handle->module->submit(std::move(native))) {
      throw std::runtime_error("native semantic worker rejected frame");
    }
    return 1;
  } catch (const std::exception& exception) {
    set_error(error, error_capacity, exception.what());
    return 0;
  }
}

int depthd_native_semantic_get_stats(
    const depthd_native_semantic_handle* handle,
    depthd_native_semantic_stats* stats, char* error,
    const std::size_t error_capacity) {
  clear_error(error, error_capacity);
  try {
    if (handle == nullptr || handle->module == nullptr) {
      throw std::invalid_argument("native semantic handle is null");
    }
    if (stats == nullptr || stats->struct_size != sizeof(*stats)) {
      throw std::invalid_argument("native semantic stats ABI size mismatch");
    }
    const auto native = handle->module->stats();
    stats->submitted_frames = native.submitted_frames;
    stats->replaced_pending_frames = native.replaced_pending_frames;
    stats->processed_frames = native.processed_frames;
    stats->published_results = native.published_results;
    stats->worker_errors = native.worker_errors;
    stats->last_frame_latency_ms = native.last_frame_latency_ms;
    stats->max_frame_latency_ms = native.max_frame_latency_ms;
    stats->pending = native.pending_frame ? 1U : 0U;
    stats->model_count = static_cast<std::uint32_t>(
        std::min<std::size_t>(native.backends.size(), 8U));
    std::fill(std::begin(stats->models), std::end(stats->models),
              depthd_native_semantic_model_stats{});
    for (std::uint32_t index = 0; index < stats->model_count; ++index) {
      const auto& source = native.backends[index];
      auto& target = stats->models[index];
      const auto size = std::min(source.model_name.size(),
                                 sizeof(target.model_name) - 1U);
      std::memcpy(target.model_name, source.model_name.data(), size);
      target.model_name[size] = '\0';
      target.completed_runs = source.completed_runs;
      target.errors = source.errors;
      target.last_latency_ms = source.last_latency_ms;
      target.max_latency_ms = source.max_latency_ms;
    }
    return 0;
  } catch (const std::exception& exception) {
    set_error(error, error_capacity, exception.what());
    return -1;
  }
}

int depthd_native_semantic_wait_for_published(
    const depthd_native_semantic_handle* handle,
    const std::uint64_t published_count, const std::uint32_t timeout_ms) {
  if (handle == nullptr || handle->module == nullptr) {
    return 0;
  }
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms);
  do {
    if (handle->module->stats().published_results >= published_count) {
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  return handle->module->stats().published_results >= published_count ? 1 : 0;
}

void depthd_native_semantic_destroy(depthd_native_semantic_handle* handle) {
  delete handle;
}

}  // extern "C"
