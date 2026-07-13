#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(DEPTHD_SEMANTIC_OUTPUT_BUILD)
#define DEPTHD_SEMANTIC_OUTPUT_API __declspec(dllexport)
#elif defined(_WIN32)
#define DEPTHD_SEMANTIC_OUTPUT_API __declspec(dllimport)
#else
#define DEPTHD_SEMANTIC_OUTPUT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct depthd_semantic_output_handle depthd_semantic_output_handle;

typedef struct depthd_semantic_output_config {
  uint32_t struct_size;
  const char* semantic_ring_path;
  const char* target_frame;
  double optical_to_target[16];
} depthd_semantic_output_config;

typedef struct depthd_semantic_observation {
  uint32_t struct_size;
  uint32_t source_kind;
  uint32_t semantic_class;
  uint32_t observation_id;
  uint32_t model_class_id;
  const char* model_label;
  const char* model_name;
  float confidence;
  float x_min;
  float y_min;
  float x_max;
  float y_max;
  uint32_t pixel_count;
} depthd_semantic_observation;

typedef struct depthd_semantic_model_run {
  uint32_t struct_size;
  const char* model_name;
  const char* model_version;
  const char* model_sha256;
  uint32_t health;
  uint64_t completed_runs;
  uint64_t errors;
  double latency_ms;
  double inference_latency_ms;
  double postprocess_latency_ms;
} depthd_semantic_model_run;

typedef struct depthd_semantic_output_frame {
  uint32_t struct_size;
  uint64_t source_sequence;
  int64_t capture_realtime_ns;
  int64_t ready_realtime_ns;
  uint32_t health;
  const float* disparity;
  uint32_t width;
  uint32_t height;
  uint32_t disparity_stride_elements;
  uint32_t valid_x_offset;
  uint32_t valid_y_offset;
  uint32_t valid_width;
  uint32_t valid_height;
  float min_disparity;
  float max_disparity;
  const depthd_semantic_observation* observations;
  uint32_t observation_count;
  const depthd_semantic_model_run* model_runs;
  uint32_t model_run_count;
  const uint8_t* preview_jpeg;
  uint32_t preview_jpeg_bytes;
  uint64_t submitted_frames;
  uint64_t replaced_pending_frames;
  uint64_t processed_frames;
  uint64_t published_results;
  uint64_t worker_errors;
  double frame_latency_ms;
  double max_frame_latency_ms;
} depthd_semantic_output_frame;

typedef struct depthd_semantic_output_stats {
  uint32_t struct_size;
  uint64_t semantic_sequence;
  uint32_t object_count;
  uint32_t hazard_active;
  uint32_t hazard_fail_closed_required;
} depthd_semantic_output_stats;

DEPTHD_SEMANTIC_OUTPUT_API void depthd_semantic_output_default_config(
    depthd_semantic_output_config* config);

DEPTHD_SEMANTIC_OUTPUT_API depthd_semantic_output_handle*
depthd_semantic_output_create(const depthd_semantic_output_config* config,
                              char* error,
                              size_t error_capacity);

DEPTHD_SEMANTIC_OUTPUT_API int depthd_semantic_output_publish(
    depthd_semantic_output_handle* handle,
    const depthd_semantic_output_frame* frame,
    depthd_semantic_output_stats* stats,
    char* error,
    size_t error_capacity);

DEPTHD_SEMANTIC_OUTPUT_API void depthd_semantic_output_destroy(
    depthd_semantic_output_handle* handle);

#ifdef __cplusplus
}
#endif
