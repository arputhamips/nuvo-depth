#include "depthd/egl_gl_dmabuf_rectification_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace depthd {
namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

std::string current_exception_message() {
  try {
    throw;
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "unknown EGL/OpenGL rectification failure";
  }
}

void validate_config(const RectificationBackendConfig& config,
                     const EglGlDmabufRectificationOptions& options) {
  if (config.input_width == 0 || config.input_height == 0 ||
      config.output_width == 0 || config.output_height == 0) {
    throw std::invalid_argument("rectification dimensions must be nonzero");
  }
  if (config.input_width != config.output_width ||
      config.input_height != config.output_height) {
    throw std::invalid_argument(
        "egl_gl_dmabuf initially requires equal input/output geometry");
  }
  if (config.interpolation != RectificationInterpolation::kLinear ||
      config.border_value != 0) {
    throw std::invalid_argument(
        "egl_gl_dmabuf requires linear sampling and a zero border");
  }
  if (options.import_cache_capacity < 2) {
    throw std::invalid_argument(
        "DMABUF import cache must hold at least a stereo pair");
  }
}

void validate_output(const RectificationBackendConfig& config,
                     const MutableImageView& output) {
  if (output.data == nullptr) {
    throw std::invalid_argument("rectification output data cannot be null");
  }
  if (output.width != config.output_width ||
      output.height != config.output_height ||
      output.encoding != ImageEncoding::kGray8) {
    throw std::invalid_argument(
        "DMABUF rectification output must be configured Gray8 geometry");
  }
  if (output.stride_bytes < output.width) {
    throw std::invalid_argument("rectification output stride is too small");
  }
}

void validate_plane(const RectificationBackendConfig& config,
                    const DmabufPlaneView& plane) {
  if (plane.fd < 0 || plane.buffer_id == 0) {
    throw std::invalid_argument(
        "DMABUF plane requires a valid fd and stable buffer identity");
  }
  if (plane.width != config.input_width ||
      plane.height != config.input_height ||
      plane.stride_bytes < plane.width ||
      plane.format != DmabufPlaneFormat::kR8) {
    throw std::invalid_argument(
        "DMABUF plane must be visible configured R8 geometry with valid stride");
  }
  if (plane.lease == nullptr || !plane.lease->valid() ||
      plane.lease->request_id() == 0) {
    throw std::invalid_argument(
        "DMABUF plane requires a live camera request lease");
  }
}

struct ImportDescription {
  int fd{-1};
  std::uint64_t buffer_id{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t stride_bytes{};
  std::uint32_t offset_bytes{};
  std::uint64_t modifier{};
  DmabufPlaneFormat format{DmabufPlaneFormat::kR8};
  DmabufFdIdentity fd_identity;

  friend bool operator==(const ImportDescription&,
                         const ImportDescription&) = default;
};

ImportDescription describe(const DmabufPlaneView& plane,
                           const EglGlDmabufPlatform& platform) {
  return ImportDescription{
      plane.fd,          plane.buffer_id,   plane.width,
      plane.height,      plane.stride_bytes, plane.offset_bytes,
      plane.modifier,    plane.format,       platform.fd_identity(plane.fd)};
}

struct CacheEntry {
  RectificationCamera camera{RectificationCamera::kLeft};
  ImportDescription description;
  EglGlImportedImage image{};
  std::uint64_t last_use{};
};

class EglGlDmabufRectificationBackend final : public RectificationBackend {
 public:
  EglGlDmabufRectificationBackend(
      RectificationBackendConfig config,
      EglGlDmabufRectificationOptions options,
      std::unique_ptr<EglGlDmabufPlatform> platform,
      std::unique_ptr<RectificationBackend> cpu_fallback,
      std::string initialization_error)
      : config_(config),
        options_(options),
        platform_(std::move(platform)),
        cpu_fallback_(std::move(cpu_fallback)) {
    validate_config(config_, options_);
    cache_.reserve(options_.import_cache_capacity);
    diagnostics_.cache_capacity = options_.import_cache_capacity;
    diagnostics_.selected_backend = std::string(
        kEglGlDmabufRectificationBackendName);

    if (platform_ == nullptr) {
      if (initialization_error.empty()) {
        initialization_error = "EGL/OpenGL platform was not constructed";
      }
      activate_fallback(std::move(initialization_error));
    }
  }

  ~EglGlDmabufRectificationBackend() override { clear_cache(); }

  [[nodiscard]] std::string_view name() const noexcept override {
    return kEglGlDmabufRectificationBackendName;
  }

  void rectify(RectificationCamera camera, const ImageView& input,
               const MutableImageView& output,
               RectificationStats& stats) override {
    if (cpu_fallback_ == nullptr) {
      throw std::runtime_error(
          "egl_gl_dmabuf accepts only paired capture-native R8 input");
    }
    cpu_fallback_->rectify(camera, input, output, stats);
    diagnostics_.last_stats = stats;
  }

  [[nodiscard]] bool supports_dmabuf_pair() const noexcept override {
    return true;
  }

  void rectify_dmabuf_pair(
      const DmabufStereoInput& input,
      const MutableImageView& left_output,
      const MutableImageView& right_output,
      RectificationStats& stats) override {
    const auto started = Clock::now();
    validate_output(config_, left_output);
    validate_output(config_, right_output);
    if (left_output.data == right_output.data) {
      throw std::invalid_argument(
          "left/right DMABUF rectification outputs cannot alias");
    }

    if (platform_ == nullptr) {
      run_cpu_fallback(input, left_output, right_output, stats);
      diagnostics_.last_stats = stats;
      return;
    }

    validate_plane(config_, input.left);
    validate_plane(config_, input.right);
    try {
      const auto left = import_for(RectificationCamera::kLeft, input.left);
      const auto right = import_for(RectificationCamera::kRight, input.right);
      double submit_wait_ms = 0.0;
      platform_->rectify_pair(left, right, left_output, right_output, stats,
                              submit_wait_ms);
      if (!input.left.lease->valid() || !input.right.lease->valid()) {
        throw std::runtime_error(
            "camera request lease ended during DMABUF rectification");
      }
      stats.total_ms = milliseconds(started);
      diagnostics_.submit_wait_ms = submit_wait_ms;
      diagnostics_.gpu_timer_supported = platform_->gpu_timer_supported();
      diagnostics_.owned_fd_count = platform_->owned_fd_count();
      diagnostics_.cache_size = cache_.size();
      diagnostics_.last_stats = stats;
    } catch (...) {
      const auto reason = current_exception_message();
      ++diagnostics_.failure_count;
      if (!options_.allow_cpu_fallback || cpu_fallback_ == nullptr) {
        throw;
      }
      clear_cache();
      platform_.reset();
      activate_fallback("GPU rectification runtime failure: " + reason);
      run_cpu_fallback(input, left_output, right_output, stats);
      stats.total_ms = milliseconds(started);
      diagnostics_.last_stats = stats;
    }
  }

  [[nodiscard]] DmabufRectificationDiagnostics dmabuf_diagnostics()
      const override {
    auto result = diagnostics_;
    result.cache_size = cache_.size();
    result.owned_fd_count =
        platform_ == nullptr ? 0 : platform_->owned_fd_count();
    return result;
  }

 private:
  EglGlImportedImage import_for(RectificationCamera camera,
                                const DmabufPlaneView& plane) {
    const auto description = describe(plane, *platform_);
    auto existing = std::find_if(
        cache_.begin(), cache_.end(), [&](const CacheEntry& entry) {
          return entry.camera == camera &&
                 entry.description.buffer_id == description.buffer_id;
        });
    if (existing != cache_.end() &&
        existing->description == description) {
      existing->last_use = ++use_sequence_;
      return existing->image;
    }

    if (existing != cache_.end()) {
      platform_->release_import(existing->image);
      cache_.erase(existing);
      ++diagnostics_.stale_reimport_count;
    }
    if (cache_.size() == options_.import_cache_capacity) {
      const auto oldest = std::min_element(
          cache_.begin(), cache_.end(),
          [](const CacheEntry& first, const CacheEntry& second) {
            return first.last_use < second.last_use;
          });
      platform_->release_import(oldest->image);
      cache_.erase(oldest);
      ++diagnostics_.eviction_count;
    }

    const auto image = platform_->import_plane(plane);
    cache_.push_back(
        CacheEntry{camera, description, image, ++use_sequence_});
    ++diagnostics_.import_count;
    diagnostics_.cache_size = cache_.size();
    return image;
  }

  void run_cpu_fallback(const DmabufStereoInput& input,
                        const MutableImageView& left_output,
                        const MutableImageView& right_output,
                        RectificationStats& stats) {
    if (!options_.allow_cpu_fallback || cpu_fallback_ == nullptr) {
      throw std::runtime_error("CPU rectification fallback is disabled");
    }
    if (input.left_cpu_fallback.data != nullptr &&
        input.right_cpu_fallback.data != nullptr) {
      cpu_fallback_->rectify_pair(input.left_cpu_fallback,
                                  input.right_cpu_fallback, left_output,
                                  right_output, stats);
      return;
    }
    if (!input.acquire_cpu_fallback) {
      throw std::runtime_error(
          "GPU fallback requires lazy CPU mappings from the active leases");
    }
    const auto fallback = input.acquire_cpu_fallback();
    if (!fallback.left_owner || !fallback.right_owner ||
        fallback.left.data == nullptr || fallback.right.data == nullptr) {
      throw std::runtime_error("lazy CPU fallback returned invalid mappings");
    }
    cpu_fallback_->rectify_pair(fallback.left, fallback.right, left_output,
                                right_output, stats);
  }

  void activate_fallback(std::string reason) {
    if (!options_.allow_cpu_fallback || cpu_fallback_ == nullptr) {
      throw std::runtime_error(reason);
    }
    diagnostics_.selected_backend = "cpu_opencv";
    diagnostics_.fallback_reason = std::move(reason);
    diagnostics_.cache_size = 0;
    diagnostics_.owned_fd_count = 0;
  }

  void clear_cache() noexcept {
    if (platform_ != nullptr) {
      for (auto& entry : cache_) {
        platform_->release_import(entry.image);
      }
    }
    cache_.clear();
    diagnostics_.cache_size = 0;
    diagnostics_.owned_fd_count =
        platform_ == nullptr ? 0 : platform_->owned_fd_count();
  }

  RectificationBackendConfig config_;
  EglGlDmabufRectificationOptions options_;
  std::unique_ptr<EglGlDmabufPlatform> platform_;
  std::unique_ptr<RectificationBackend> cpu_fallback_;
  std::vector<CacheEntry> cache_;
  std::uint64_t use_sequence_{};
  DmabufRectificationDiagnostics diagnostics_;
};

}  // namespace

std::unique_ptr<RectificationBackend>
make_egl_gl_dmabuf_rectification_backend(
    const RectificationBackendConfig& config,
    EglGlDmabufRectificationOptions options,
    std::unique_ptr<EglGlDmabufPlatform> platform,
    std::unique_ptr<RectificationBackend> cpu_fallback,
    std::string initialization_error) {
  return std::make_unique<EglGlDmabufRectificationBackend>(
      config, options, std::move(platform), std::move(cpu_fallback),
      std::move(initialization_error));
}

}  // namespace depthd
