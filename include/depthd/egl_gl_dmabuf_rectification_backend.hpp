#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "depthd/rectification_backend.hpp"

namespace depthd {

inline constexpr std::string_view kEglGlDmabufRectificationBackendName =
    "egl_gl_dmabuf";

struct EglGlDmabufRectificationOptions {
  std::size_t import_cache_capacity{8};
  bool allow_cpu_fallback{true};
};

struct DmabufFdIdentity {
  std::uint64_t device{};
  std::uint64_t inode{};

  friend bool operator==(const DmabufFdIdentity&,
                         const DmabufFdIdentity&) = default;
};

using EglGlImportedImage = std::uint64_t;

// Injectable platform seam. The dependency-free cache/lifecycle policy is
// tested with a fake; the production implementation owns EGL, GLES, duplicated
// fds, imported EGLImages, resident map textures, and fixed output resources.
class EglGlDmabufPlatform {
 public:
  virtual ~EglGlDmabufPlatform() = default;

  [[nodiscard]] virtual DmabufFdIdentity fd_identity(int fd) const = 0;
  [[nodiscard]] virtual EglGlImportedImage import_plane(
      const DmabufPlaneView& plane) = 0;
  virtual void release_import(EglGlImportedImage image) noexcept = 0;

  virtual void rectify_pair(
      EglGlImportedImage left, EglGlImportedImage right,
      const MutableImageView& left_output,
      const MutableImageView& right_output, RectificationStats& stats,
      double& submit_wait_ms) = 0;

  [[nodiscard]] virtual std::size_t owned_fd_count() const noexcept = 0;
  [[nodiscard]] virtual bool gpu_timer_supported() const noexcept = 0;
};

// Used by the fake-platform tests and by the production registration factory.
// An empty platform plus a non-empty initialization_error represents a clean
// initialization fallback; it is rejected if fallback is disabled.
[[nodiscard]] std::unique_ptr<RectificationBackend>
make_egl_gl_dmabuf_rectification_backend(
    const RectificationBackendConfig& config,
    EglGlDmabufRectificationOptions options,
    std::unique_ptr<EglGlDmabufPlatform> platform,
    std::unique_ptr<RectificationBackend> cpu_fallback,
    std::string initialization_error = {});

// Available only from the optional EGL/GLES module. Registration is explicit
// and captures the fallback/cache policy chosen by the orchestrator.
void register_egl_gl_dmabuf_rectification_backend(
    RectificationBackendRegistry& registry,
    EglGlDmabufRectificationOptions options = {});

}  // namespace depthd
