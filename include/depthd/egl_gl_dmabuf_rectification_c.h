#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct depthd_egl_gl_rectifier depthd_egl_gl_rectifier;

typedef struct depthd_dmabuf_luma_plane {
  int fd;
  uint64_t buffer_id;
  uint64_t request_id;
  uint32_t width;
  uint32_t height;
  uint32_t stride_bytes;
  uint32_t offset_bytes;
  uint64_t modifier;
} depthd_dmabuf_luma_plane;

typedef struct depthd_rectification_timing {
  double input_transfer_ms;
  double compute_ms;
  double output_transfer_ms;
  double total_ms;
  double submit_wait_ms;
} depthd_rectification_timing;

typedef struct depthd_dmabuf_rectification_diagnostics {
  uint64_t cache_size;
  uint64_t cache_capacity;
  uint64_t import_count;
  uint64_t stale_reimport_count;
  uint64_t eviction_count;
  uint64_t failure_count;
  uint64_t owned_fd_count;
  int gpu_timer_supported;
  int gpu_active;
} depthd_dmabuf_rectification_diagnostics;

// Dense maps contain width*height float32 entries each. Creation copies them;
// the caller may release its arrays after this function returns.
depthd_egl_gl_rectifier* depthd_egl_gl_rectifier_create(
    uint32_t width, uint32_t height, const float* left_source_x,
    const float* left_source_y, const float* right_source_x,
    const float* right_source_y, size_t import_cache_capacity,
    int allow_cpu_fallback);

// cpu_left/right are optional zero-copy mappings of the same leased planes.
// They are ignored while the GPU is healthy and are required only after a
// configured fallback. Outputs are caller-owned packed/padded Gray8 images.
int depthd_egl_gl_rectifier_rectify_pair(
    depthd_egl_gl_rectifier* rectifier,
    const depthd_dmabuf_luma_plane* left,
    const depthd_dmabuf_luma_plane* right, const uint8_t* cpu_left,
    size_t cpu_left_stride, const uint8_t* cpu_right,
    size_t cpu_right_stride, uint8_t* left_output,
    size_t left_output_stride, uint8_t* right_output,
    size_t right_output_stride, depthd_rectification_timing* timing);

int depthd_egl_gl_rectifier_diagnostics(
    const depthd_egl_gl_rectifier* rectifier,
    depthd_dmabuf_rectification_diagnostics* diagnostics);

const char* depthd_egl_gl_rectifier_selected_backend(
    const depthd_egl_gl_rectifier* rectifier);
const char* depthd_egl_gl_rectifier_fallback_reason(
    const depthd_egl_gl_rectifier* rectifier);
const char* depthd_egl_gl_rectifier_last_error(void);
void depthd_egl_gl_rectifier_destroy(depthd_egl_gl_rectifier* rectifier);

#ifdef __cplusplus
}
#endif
