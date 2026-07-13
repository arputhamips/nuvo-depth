#include "depthd/semantic_output_c.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "depthd/native_output.hpp"
#include "depthd/semantic_fusion.hpp"
#include "depthd/semantic_ring.hpp"

struct depthd_semantic_output_handle {
  depthd::StereoCameraModel camera{};
  depthd::RigidTransform optical_to_target{};
  depthd::SemanticDepthFusion fusion{};
  std::unique_ptr<depthd::SemanticRingWriter> writer{};
};

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

std::string required_string(const char* value, const char* field) {
  if (value == nullptr) {
    throw std::invalid_argument(std::string(field) + " is null");
  }
  return value;
}

bool valid_health(const std::uint32_t value) noexcept {
  return value <=
      static_cast<std::uint32_t>(depthd::SemanticHealthState::kFault);
}

}  // namespace

extern "C" {

void depthd_semantic_output_default_config(
    depthd_semantic_output_config* config) {
  if (config == nullptr) {
    return;
  }
  std::memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  const auto transform = depthd::make_nuvo_left_optical_to_base_transform();
  std::copy(transform.matrix.begin(), transform.matrix.end(),
            config->optical_to_target);
}

depthd_semantic_output_handle* depthd_semantic_output_create(
    const depthd_semantic_output_config* config, char* error,
    const std::size_t error_capacity) {
  clear_error(error, error_capacity);
  try {
    if (config == nullptr || config->struct_size != sizeof(*config)) {
      throw std::invalid_argument("semantic output config ABI size mismatch");
    }
    depthd::SemanticRingConfig ring;
    if (config->semantic_ring_path != nullptr &&
        config->semantic_ring_path[0] != '\0') {
      ring.path = config->semantic_ring_path;
    }
    if (config->target_frame != nullptr && config->target_frame[0] != '\0') {
      ring.target_frame = config->target_frame;
    }
    auto handle = std::make_unique<depthd_semantic_output_handle>();
    std::copy(std::begin(config->optical_to_target),
              std::end(config->optical_to_target),
              handle->optical_to_target.matrix.begin());
    handle->writer =
        std::make_unique<depthd::SemanticRingWriter>(std::move(ring));
    return handle.release();
  } catch (const std::exception& exception) {
    set_error(error, error_capacity, exception.what());
    return nullptr;
  }
}

int depthd_semantic_output_publish(depthd_semantic_output_handle* handle,
                                   const depthd_semantic_output_frame* frame,
                                   depthd_semantic_output_stats* stats,
                                   char* error,
                                   const std::size_t error_capacity) {
  clear_error(error, error_capacity);
  try {
    if (handle == nullptr || handle->writer == nullptr) {
      throw std::invalid_argument("semantic output handle is null");
    }
    if (frame == nullptr || frame->struct_size != sizeof(*frame)) {
      throw std::invalid_argument("semantic output frame ABI size mismatch");
    }
    if (stats == nullptr || stats->struct_size != sizeof(*stats)) {
      throw std::invalid_argument("semantic output stats ABI size mismatch");
    }
    if (frame->disparity == nullptr || frame->width != 728U ||
        frame->height != 544U ||
        frame->disparity_stride_elements < frame->width) {
      throw std::invalid_argument("semantic output disparity is invalid");
    }
    if (frame->observation_count > 0U && frame->observations == nullptr) {
      throw std::invalid_argument("semantic observations pointer is null");
    }
    if (frame->model_run_count > 0U && frame->model_runs == nullptr) {
      throw std::invalid_argument("semantic model-run pointer is null");
    }
    if (frame->preview_jpeg_bytes > 0U && frame->preview_jpeg == nullptr) {
      throw std::invalid_argument("semantic preview pointer is null");
    }
    if (!valid_health(frame->health)) {
      throw std::invalid_argument("semantic health value is invalid");
    }

    std::vector<depthd::SemanticObservation> observations;
    observations.reserve(frame->observation_count);
    for (std::uint32_t index = 0; index < frame->observation_count; ++index) {
      const auto& source = frame->observations[index];
      if (source.struct_size != sizeof(source) || source.source_kind < 1U ||
          source.source_kind > 2U) {
        throw std::invalid_argument("semantic observation ABI is invalid");
      }
      depthd::SemanticObservation target;
      target.source_kind = static_cast<depthd::SemanticSourceKind>(
          source.source_kind);
      target.semantic_class = static_cast<depthd::SemanticClass>(
          source.semantic_class);
      target.observation_id = source.observation_id;
      target.model_class_id = source.model_class_id;
      target.model_label = required_string(source.model_label, "model label");
      target.model_name = required_string(source.model_name, "model name");
      target.confidence = source.confidence;
      target.box = {source.x_min, source.y_min, source.x_max, source.y_max};
      target.pixel_count = source.pixel_count;
      observations.push_back(std::move(target));
    }

    depthd::DisparityFrameView disparity{
        frame->disparity,
        frame->width,
        frame->height,
        frame->disparity_stride_elements,
        frame->valid_x_offset,
        frame->valid_y_offset,
        frame->valid_width,
        frame->valid_height,
        frame->min_disparity,
        frame->max_disparity,
    };
    depthd::SemanticFrameResult result;
    result.source_sequence = frame->source_sequence;
    result.capture_realtime_ns = frame->capture_realtime_ns;
    result.ready_realtime_ns = frame->ready_realtime_ns;
    result.health = static_cast<depthd::SemanticHealthState>(frame->health);
    result.objects = handle->fusion.fuse_all(
        observations, disparity, handle->camera, handle->optical_to_target);
    result.hazard = handle->fusion.evaluate_hazard(
        result.objects, result.health != depthd::SemanticHealthState::kOk);
    result.models.reserve(frame->model_run_count);
    for (std::uint32_t index = 0; index < frame->model_run_count; ++index) {
      const auto& source = frame->model_runs[index];
      if (source.struct_size != sizeof(source) ||
          !valid_health(source.health)) {
        throw std::invalid_argument("semantic model-run ABI is invalid");
      }
      result.models.push_back({
          required_string(source.model_name, "model run name"),
          required_string(source.model_version, "model run version"),
          required_string(source.model_sha256, "model run SHA-256"),
          static_cast<depthd::SemanticHealthState>(source.health),
          source.completed_runs,
          source.errors,
          source.latency_ms,
          source.inference_latency_ms,
          source.postprocess_latency_ms,
      });
    }
    depthd::SemanticPerceptionStats module_stats;
    module_stats.submitted_frames = frame->submitted_frames;
    module_stats.replaced_pending_frames = frame->replaced_pending_frames;
    module_stats.processed_frames = frame->processed_frames;
    module_stats.published_results = frame->published_results;
    module_stats.worker_errors = frame->worker_errors;
    module_stats.last_frame_latency_ms = frame->frame_latency_ms;
    module_stats.max_frame_latency_ms = frame->max_frame_latency_ms;
    const auto preview = std::span<const std::uint8_t>(
        frame->preview_jpeg, frame->preview_jpeg_bytes);
    const auto sequence = handle->writer->publish({&result, &module_stats, preview});
    stats->semantic_sequence = sequence;
    stats->object_count = static_cast<std::uint32_t>(result.objects.size());
    stats->hazard_active = result.hazard.active ? 1U : 0U;
    stats->hazard_fail_closed_required =
        result.hazard.fail_closed_required ? 1U : 0U;
    return 0;
  } catch (const std::exception& exception) {
    set_error(error, error_capacity, exception.what());
    return -1;
  }
}

void depthd_semantic_output_destroy(depthd_semantic_output_handle* handle) {
  delete handle;
}

}  // extern "C"
