#include "depthd/egl_gl_dmabuf_rectification_c.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "depthd/egl_gl_dmabuf_rectification_backend.hpp"

namespace {

thread_local std::string last_error;

class ActiveLease final : public depthd::DmabufRequestLease {
 public:
  explicit ActiveLease(std::uint64_t request_id) : request_id_(request_id) {}
  [[nodiscard]] bool valid() const noexcept override { return true; }
  [[nodiscard]] std::uint64_t request_id() const noexcept override {
    return request_id_;
  }

 private:
  std::uint64_t request_id_;
};

depthd::RectificationMap copy_map(std::uint32_t width, std::uint32_t height,
                                 const float* source_x,
                                 const float* source_y) {
  if (source_x == nullptr || source_y == nullptr) {
    throw std::invalid_argument("rectification map pointer is null");
  }
  const auto count = static_cast<std::size_t>(width) * height;
  depthd::RectificationMap map;
  map.width = width;
  map.height = height;
  map.source_x.assign(source_x, source_x + count);
  map.source_y.assign(source_y, source_y + count);
  return map;
}

depthd::DmabufPlaneView make_plane(const depthd_dmabuf_luma_plane& source,
                                   const ActiveLease& lease) {
  return depthd::DmabufPlaneView{
      source.fd,
      source.buffer_id,
      source.width,
      source.height,
      source.stride_bytes,
      source.offset_bytes,
      source.modifier,
      depthd::DmabufPlaneFormat::kR8,
      &lease,
  };
}

}  // namespace

struct depthd_egl_gl_rectifier {
  std::uint32_t width{};
  std::uint32_t height{};
  std::unique_ptr<depthd::RectificationBackend> backend;
  mutable std::string selected_backend;
  mutable std::string fallback_reason;
};

extern "C" depthd_egl_gl_rectifier* depthd_egl_gl_rectifier_create(
    uint32_t width, uint32_t height, const float* left_source_x,
    const float* left_source_y, const float* right_source_x,
    const float* right_source_y, size_t import_cache_capacity,
    int allow_cpu_fallback) {
  try {
    if (width == 0 || height == 0) {
      throw std::invalid_argument("rectification dimensions must be nonzero");
    }
    depthd::StereoRectificationMaps maps;
    maps.left = copy_map(width, height, left_source_x, left_source_y);
    maps.right = copy_map(width, height, right_source_x, right_source_y);
    depthd::RectificationBackendConfig config;
    config.input_width = width;
    config.input_height = height;
    config.output_width = width;
    config.output_height = height;

    depthd::EglGlDmabufRectificationOptions options;
    options.import_cache_capacity = import_cache_capacity;
    options.allow_cpu_fallback = allow_cpu_fallback != 0;
    depthd::RectificationBackendRegistry registry;
    depthd::register_egl_gl_dmabuf_rectification_backend(registry, options);

    auto result = std::make_unique<depthd_egl_gl_rectifier>();
    result->width = width;
    result->height = height;
    result->backend = registry.create(
        depthd::kEglGlDmabufRectificationBackendName, {}, false, config, maps);
    last_error.clear();
    return result.release();
  } catch (const std::exception& error) {
    last_error = error.what();
    return nullptr;
  } catch (...) {
    last_error = "unknown EGL/OpenGL rectifier creation failure";
    return nullptr;
  }
}

extern "C" int depthd_egl_gl_rectifier_rectify_pair(
    depthd_egl_gl_rectifier* rectifier,
    const depthd_dmabuf_luma_plane* left,
    const depthd_dmabuf_luma_plane* right, const uint8_t* cpu_left,
    size_t cpu_left_stride, const uint8_t* cpu_right,
    size_t cpu_right_stride, uint8_t* left_output,
    size_t left_output_stride, uint8_t* right_output,
    size_t right_output_stride, depthd_rectification_timing* timing) {
  try {
    if (rectifier == nullptr || left == nullptr || right == nullptr ||
        left_output == nullptr || right_output == nullptr || timing == nullptr) {
      throw std::invalid_argument("rectification call pointer is null");
    }
    ActiveLease left_lease(left->request_id);
    ActiveLease right_lease(right->request_id);
    depthd::DmabufStereoInput input;
    input.left = make_plane(*left, left_lease);
    input.right = make_plane(*right, right_lease);
    input.left_cpu_fallback = depthd::ImageView{
        cpu_left, rectifier->width, rectifier->height, cpu_left_stride,
        depthd::ImageEncoding::kGray8};
    input.right_cpu_fallback = depthd::ImageView{
        cpu_right, rectifier->width, rectifier->height, cpu_right_stride,
        depthd::ImageEncoding::kGray8};
    const depthd::MutableImageView left_view{
        left_output, rectifier->width, rectifier->height, left_output_stride,
        depthd::ImageEncoding::kGray8};
    const depthd::MutableImageView right_view{
        right_output, rectifier->width, rectifier->height, right_output_stride,
        depthd::ImageEncoding::kGray8};

    depthd::RectificationStats stats;
    rectifier->backend->rectify_dmabuf_pair(input, left_view, right_view,
                                             stats);
    const auto diagnostics = rectifier->backend->dmabuf_diagnostics();
    *timing = depthd_rectification_timing{
        stats.input_transfer_ms, stats.compute_ms, stats.output_transfer_ms,
        stats.total_ms, diagnostics.submit_wait_ms};
    last_error.clear();
    return 0;
  } catch (const std::exception& error) {
    last_error = error.what();
    return -1;
  } catch (...) {
    last_error = "unknown EGL/OpenGL rectification failure";
    return -1;
  }
}

extern "C" int depthd_egl_gl_rectifier_diagnostics(
    const depthd_egl_gl_rectifier* rectifier,
    depthd_dmabuf_rectification_diagnostics* output) {
  if (rectifier == nullptr || output == nullptr) {
    last_error = "diagnostics pointer is null";
    return -1;
  }
  const auto diagnostics = rectifier->backend->dmabuf_diagnostics();
  *output = depthd_dmabuf_rectification_diagnostics{
      diagnostics.cache_size,
      diagnostics.cache_capacity,
      diagnostics.import_count,
      diagnostics.stale_reimport_count,
      diagnostics.eviction_count,
      diagnostics.failure_count,
      diagnostics.owned_fd_count,
      diagnostics.gpu_timer_supported ? 1 : 0,
      diagnostics.selected_backend ==
              depthd::kEglGlDmabufRectificationBackendName
          ? 1
          : 0,
  };
  return 0;
}

extern "C" const char* depthd_egl_gl_rectifier_selected_backend(
    const depthd_egl_gl_rectifier* rectifier) {
  if (rectifier == nullptr) {
    return "";
  }
  rectifier->selected_backend =
      rectifier->backend->dmabuf_diagnostics().selected_backend;
  return rectifier->selected_backend.c_str();
}

extern "C" const char* depthd_egl_gl_rectifier_fallback_reason(
    const depthd_egl_gl_rectifier* rectifier) {
  if (rectifier == nullptr) {
    return "";
  }
  rectifier->fallback_reason =
      rectifier->backend->dmabuf_diagnostics().fallback_reason;
  return rectifier->fallback_reason.c_str();
}

extern "C" const char* depthd_egl_gl_rectifier_last_error(void) {
  return last_error.c_str();
}

extern "C" void depthd_egl_gl_rectifier_destroy(
    depthd_egl_gl_rectifier* rectifier) {
  delete rectifier;
}
