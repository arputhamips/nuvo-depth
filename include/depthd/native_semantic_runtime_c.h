#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(DEPTHD_NATIVE_SEMANTIC_BUILD)
#define DEPTHD_NATIVE_SEMANTIC_API __declspec(dllexport)
#elif defined(_WIN32)
#define DEPTHD_NATIVE_SEMANTIC_API __declspec(dllimport)
#else
#define DEPTHD_NATIVE_SEMANTIC_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct depthd_native_semantic_handle depthd_native_semantic_handle;

typedef void (*depthd_native_semantic_release_fn)(void* context,
                                                   uint64_t lease_token);

typedef struct depthd_native_semantic_config {
  uint32_t struct_size;
  const char* semantic_ring_path;
  const char* target_frame;
  const char* yolo_hef_path;
  const char* segformer_hef_path;
  double yolo_hz;
  double segformer_hz;
  uint32_t preview_enabled;
  uint32_t preview_jpeg_quality;
  depthd_native_semantic_release_fn release_frame;
  void* release_context;
  double optical_to_target[16];
} depthd_native_semantic_config;

typedef struct depthd_native_semantic_frame {
  uint32_t struct_size;
  uint64_t lease_token;
  uint64_t source_sequence;
  int64_t capture_realtime_ns;
  int64_t submitted_steady_ns;
  const uint8_t* left_rectified_bgr;
  uint32_t width;
  uint32_t height;
  uint32_t color_stride_bytes;
  const float* disparity;
  uint32_t disparity_stride_elements;
  uint32_t valid_x_offset;
  uint32_t valid_y_offset;
  uint32_t valid_width;
  uint32_t valid_height;
  float min_disparity;
  float max_disparity;
} depthd_native_semantic_frame;

typedef struct depthd_native_semantic_model_stats {
  char model_name[48];
  uint64_t completed_runs;
  uint64_t errors;
  double last_latency_ms;
  double max_latency_ms;
} depthd_native_semantic_model_stats;

typedef struct depthd_native_semantic_stats {
  uint32_t struct_size;
  uint64_t submitted_frames;
  uint64_t replaced_pending_frames;
  uint64_t processed_frames;
  uint64_t published_results;
  uint64_t worker_errors;
  double last_frame_latency_ms;
  double max_frame_latency_ms;
  uint32_t pending;
  uint32_t model_count;
  depthd_native_semantic_model_stats models[8];
} depthd_native_semantic_stats;

DEPTHD_NATIVE_SEMANTIC_API void depthd_native_semantic_default_config(
    depthd_native_semantic_config* config);

DEPTHD_NATIVE_SEMANTIC_API int depthd_native_semantic_hailort_available(void);

DEPTHD_NATIVE_SEMANTIC_API depthd_native_semantic_handle*
depthd_native_semantic_create(const depthd_native_semantic_config* config,
                              char* error,
                              size_t error_capacity);

// Returns one when at least one registered model is due and reserves one
// max-model-cadence submission slot. The intended call pattern is an immediate
// submit using the same timestamp.
DEPTHD_NATIVE_SEMANTIC_API int depthd_native_semantic_wants_frame(
    const depthd_native_semantic_handle* handle,
    int64_t submitted_steady_ns);

// Capacity-one latest-only submission. On every return path, ownership of the
// lease token transfers to this API and release_frame is called exactly once.
DEPTHD_NATIVE_SEMANTIC_API int depthd_native_semantic_submit(
    depthd_native_semantic_handle* handle,
    const depthd_native_semantic_frame* frame,
    char* error,
    size_t error_capacity);

DEPTHD_NATIVE_SEMANTIC_API int depthd_native_semantic_get_stats(
    const depthd_native_semantic_handle* handle,
    depthd_native_semantic_stats* stats,
    char* error,
    size_t error_capacity);

DEPTHD_NATIVE_SEMANTIC_API int depthd_native_semantic_wait_for_published(
    const depthd_native_semantic_handle* handle,
    uint64_t published_count,
    uint32_t timeout_ms);

DEPTHD_NATIVE_SEMANTIC_API void depthd_native_semantic_destroy(
    depthd_native_semantic_handle* handle);

#ifdef __cplusplus
}
#endif
