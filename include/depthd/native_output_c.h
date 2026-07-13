#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(DEPTHD_NATIVE_OUTPUT_BUILD)
#define DEPTHD_NATIVE_OUTPUT_API __declspec(dllexport)
#elif defined(_WIN32)
#define DEPTHD_NATIVE_OUTPUT_API __declspec(dllimport)
#else
#define DEPTHD_NATIVE_OUTPUT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct depthd_native_output_handle depthd_native_output_handle;

typedef struct depthd_native_output_config {
  uint32_t struct_size;
  const char* result_ring_path;
  double optical_to_base[16];
} depthd_native_output_config;

typedef struct depthd_native_output_frame {
  uint32_t struct_size;
  int64_t capture_realtime_ns;
  uint32_t health;
  const float* disparity;
  uint32_t width;
  uint32_t height;
  uint32_t disparity_stride_elements;
  const uint8_t* left_rectified_mono;
  uint32_t left_rectified_mono_bytes;
  const uint8_t* left_rectified_jpeg;
  uint32_t left_rectified_jpeg_bytes;
} depthd_native_output_frame;

typedef struct depthd_native_output_stats {
  uint32_t struct_size;
  uint64_t sequence;
  uint32_t obstacle_point_count;
  double obstacle_cloud_ms;
  double result_ring_publish_ms;
  double total_ms;
  uint64_t sampled_pixels;
  uint64_t accepted_raw_points;
  uint64_t accumulated_voxels;
  uint64_t supported_voxels;
  uint64_t accepted_clusters;
  uint64_t voxel_capacity_overflow;
} depthd_native_output_stats;

DEPTHD_NATIVE_OUTPUT_API void depthd_native_output_default_config(
    depthd_native_output_config* config);

DEPTHD_NATIVE_OUTPUT_API depthd_native_output_handle*
depthd_native_output_create(const depthd_native_output_config* config,
                            char* error,
                            size_t error_capacity);

DEPTHD_NATIVE_OUTPUT_API int depthd_native_output_publish(
    depthd_native_output_handle* handle,
    const depthd_native_output_frame* frame,
    depthd_native_output_stats* stats,
    char* error,
    size_t error_capacity);

DEPTHD_NATIVE_OUTPUT_API void depthd_native_output_destroy(
    depthd_native_output_handle* handle);

#ifdef __cplusplus
}
#endif
