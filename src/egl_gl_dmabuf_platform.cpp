#include "depthd/egl_gl_dmabuf_rectification_backend.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "depthd/opencv_rectification_backend.hpp"

namespace depthd {
namespace {

using Clock = std::chrono::steady_clock;

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif

constexpr std::uint32_t fourcc(char first, char second, char third,
                               char fourth) {
  return static_cast<std::uint32_t>(first) |
         (static_cast<std::uint32_t>(second) << 8U) |
         (static_cast<std::uint32_t>(third) << 16U) |
         (static_cast<std::uint32_t>(fourth) << 24U);
}

constexpr std::uint32_t kDrmFormatR8 = fourcc('R', '8', ' ', ' ');

double milliseconds(Clock::time_point started) {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

bool has_extension(const char* extensions, std::string_view requested) {
  if (extensions == nullptr || requested.empty() ||
      requested.find(' ') != std::string_view::npos) {
    return false;
  }
  std::string_view all(extensions);
  std::size_t offset = 0;
  while (offset < all.size()) {
    const auto end = all.find(' ', offset);
    const auto token = all.substr(
        offset, end == std::string_view::npos ? all.size() - offset
                                              : end - offset);
    if (token == requested) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1;
  }
  return false;
}

void require_egl(bool condition, std::string_view operation) {
  if (!condition) {
    throw std::runtime_error(std::string(operation) +
                             " failed (EGL error " +
                             std::to_string(eglGetError()) + ")");
  }
}

void require_gl(std::string_view operation) {
  const auto error = glGetError();
  if (error != GL_NO_ERROR) {
    throw std::runtime_error(std::string(operation) +
                             " failed (GL error " +
                             std::to_string(error) + ")");
  }
}

GLuint compile_shader(GLenum type, const char* source) {
  const auto shader = glCreateShader(type);
  if (shader == 0) {
    throw std::runtime_error("glCreateShader failed");
  }
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != GL_TRUE) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    throw std::runtime_error("rectification shader compile failed: " + log);
  }
  return shader;
}

GLuint make_program() {
  static constexpr char kVertexShader[] = R"glsl(#version 300 es
void main() {
  const vec2 vertices[3] = vec2[3](
      vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
  gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);
}
)glsl";

  static constexpr char kFragmentShader[] = R"glsl(#version 300 es
precision highp float;
precision highp int;

uniform highp sampler2D u_left_input;
uniform highp sampler2D u_right_input;
uniform highp sampler2D u_left_map;
uniform highp sampler2D u_right_map;
uniform ivec2 u_input_size;
uniform int u_output_width;
layout(location = 0) out highp float out_luma;

float fetch_or_zero(highp sampler2D image, ivec2 pixel) {
  if (pixel.x < 0 || pixel.y < 0 ||
      pixel.x >= u_input_size.x || pixel.y >= u_input_size.y) {
    return 0.0;
  }
  return texelFetch(image, pixel, 0).r;
}

float remap(highp sampler2D image, vec2 source) {
  ivec2 base = ivec2(floor(source));
  vec2 fraction = source - vec2(base);
  float top = mix(fetch_or_zero(image, base),
                  fetch_or_zero(image, base + ivec2(1, 0)), fraction.x);
  float bottom = mix(fetch_or_zero(image, base + ivec2(0, 1)),
                     fetch_or_zero(image, base + ivec2(1, 1)), fraction.x);
  return mix(top, bottom, fraction.y);
}

void main() {
  ivec2 output_pixel = ivec2(gl_FragCoord.xy);
  bool right = output_pixel.x >= u_output_width;
  ivec2 camera_pixel = ivec2(
      right ? output_pixel.x - u_output_width : output_pixel.x,
      output_pixel.y);
  vec2 source = right ? texelFetch(u_right_map, camera_pixel, 0).rg
                      : texelFetch(u_left_map, camera_pixel, 0).rg;
  out_luma = right ? remap(u_right_input, source)
                   : remap(u_left_input, source);
}
)glsl";

  const auto vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader);
  const auto fragment = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
  const auto program = glCreateProgram();
  if (program == 0) {
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    throw std::runtime_error("glCreateProgram failed");
  }
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    glDeleteProgram(program);
    throw std::runtime_error("rectification program link failed: " + log);
  }
  return program;
}

std::vector<float> pack_quantized_map(const RectificationMap& map) {
  const auto pixel_count = static_cast<std::size_t>(map.width) * map.height;
  std::vector<float> packed(pixel_count * 2U);
  for (std::size_t index = 0; index < pixel_count; ++index) {
    // Match OpenCV INTER_BITS=5. nearbyint follows the default round-to-even
    // mode used by cvRound on AArch64 for the tie cases.
    packed[index * 2U] = std::nearbyint(map.source_x[index] * 32.0F) / 32.0F;
    packed[index * 2U + 1U] =
        std::nearbyint(map.source_y[index] * 32.0F) / 32.0F;
  }
  return packed;
}

struct ImportedResource {
  EglGlImportedImage handle{};
  EGLImageKHR egl_image{EGL_NO_IMAGE_KHR};
  GLuint texture{};
  int owned_fd{-1};
};

class MesaEglGlDmabufPlatform final : public EglGlDmabufPlatform {
 public:
  MesaEglGlDmabufPlatform(
      const RectificationBackendConfig& config,
      const StereoRectificationMaps& maps, std::size_t cache_capacity)
      : config_(config),
        readback_(static_cast<std::size_t>(config.output_width) * 2U *
                  config.output_height) {
    if (!is_valid(maps) || maps.left.width != config.output_width ||
        maps.left.height != config.output_height) {
      throw std::invalid_argument(
          "EGL/OpenGL rectification maps do not match output geometry");
    }
    imports_.reserve(cache_capacity);
    try {
      initialize_egl();
      initialize_gl(maps);
    } catch (...) {
      shutdown();
      throw;
    }
  }

  ~MesaEglGlDmabufPlatform() override { shutdown(); }

  [[nodiscard]] DmabufFdIdentity fd_identity(int fd) const override {
    struct stat status {};
    if (fstat(fd, &status) != 0) {
      throw std::runtime_error("fstat failed for camera DMABUF");
    }
    return DmabufFdIdentity{static_cast<std::uint64_t>(status.st_dev),
                            static_cast<std::uint64_t>(status.st_ino)};
  }

  [[nodiscard]] EglGlImportedImage import_plane(
      const DmabufPlaneView& plane) override {
    make_current();
    if (plane.format != DmabufPlaneFormat::kR8) {
      throw std::invalid_argument("only YUV420 Y-plane R8 import is supported");
    }
    const int owned_fd = fcntl(plane.fd, F_DUPFD_CLOEXEC, 0);
    if (owned_fd < 0) {
      throw std::runtime_error("could not duplicate camera DMABUF fd");
    }

    std::vector<EGLint> attributes = {
        EGL_WIDTH,
        static_cast<EGLint>(plane.width),
        EGL_HEIGHT,
        static_cast<EGLint>(plane.height),
        EGL_LINUX_DRM_FOURCC_EXT,
        static_cast<EGLint>(kDrmFormatR8),
        EGL_DMA_BUF_PLANE0_FD_EXT,
        owned_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        static_cast<EGLint>(plane.offset_bytes),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        static_cast<EGLint>(plane.stride_bytes),
    };
    if (plane.modifier != kDmabufModifierInvalid) {
      if (!modifier_import_supported_) {
        close(owned_fd);
        throw std::runtime_error(
            "DMABUF has a modifier but EGL modifier import is unavailable");
      }
      attributes.insert(attributes.end(), {
          EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
          static_cast<EGLint>(plane.modifier & 0xffffffffULL),
          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
          static_cast<EGLint>(plane.modifier >> 32U),
      });
    }
    attributes.push_back(EGL_NONE);

    const auto image = create_image_(
        display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr,
        attributes.data());
    if (image == EGL_NO_IMAGE_KHR) {
      const auto error = eglGetError();
      close(owned_fd);
      throw std::runtime_error("EGL R8 DMABUF import failed (error " +
                               std::to_string(error) + ")");
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target_texture_(GL_TEXTURE_2D, image);
    const auto error = glGetError();
    if (texture == 0 || error != GL_NO_ERROR) {
      if (texture != 0) {
        glDeleteTextures(1, &texture);
      }
      destroy_image_(display_, image);
      close(owned_fd);
      throw std::runtime_error("glEGLImageTargetTexture2DOES R8 failed");
    }

    const auto handle = next_handle_++;
    imports_.push_back(ImportedResource{handle, image, texture, owned_fd});
    return handle;
  }

  void release_import(EglGlImportedImage image) noexcept override {
    const auto found = std::find_if(
        imports_.begin(), imports_.end(),
        [image](const ImportedResource& resource) {
          return resource.handle == image;
        });
    if (found == imports_.end()) {
      return;
    }
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
      static_cast<void>(eglMakeCurrent(display_, surface_, surface_, context_));
      if (found->texture != 0) {
        glDeleteTextures(1, &found->texture);
      }
      if (found->egl_image != EGL_NO_IMAGE_KHR && destroy_image_ != nullptr) {
        static_cast<void>(destroy_image_(display_, found->egl_image));
      }
    }
    if (found->owned_fd >= 0) {
      close(found->owned_fd);
    }
    imports_.erase(found);
  }

  void rectify_pair(EglGlImportedImage left, EglGlImportedImage right,
                    const MutableImageView& left_output,
                    const MutableImageView& right_output,
                    RectificationStats& stats,
                    double& submit_wait_ms) override {
    const auto total_started = Clock::now();
    make_current();
    const auto* left_resource = find_import(left);
    const auto* right_resource = find_import(right);
    if (left_resource == nullptr || right_resource == nullptr) {
      throw std::runtime_error("stale EGLImage import handle");
    }

    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, left_resource->texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, right_resource->texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, left_map_texture_);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, right_map_texture_);
    glBindFramebuffer(GL_FRAMEBUFFER, output_framebuffer_);
    glViewport(0, 0, static_cast<GLsizei>(config_.output_width * 2U),
               static_cast<GLsizei>(config_.output_height));

    if (timer_supported_) {
      begin_query_(GL_TIME_ELAPSED_EXT, timer_query_);
    }
    const auto submit_started = Clock::now();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (timer_supported_) {
      end_query_(GL_TIME_ELAPSED_EXT);
    }
    glFlush();
    glFinish();
    submit_wait_ms = milliseconds(submit_started);
    require_gl("paired rectification draw");

    std::uint64_t gpu_nanoseconds = 0;
    if (timer_supported_) {
      get_query_ui64_(timer_query_, GL_QUERY_RESULT_EXT, &gpu_nanoseconds);
      GLint disjoint = GL_FALSE;
      glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
      if (disjoint == GL_TRUE) {
        gpu_nanoseconds = 0;
      }
    }

    const auto output_started = Clock::now();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, static_cast<GLsizei>(config_.output_width * 2U),
                 static_cast<GLsizei>(config_.output_height), GL_RED,
                 GL_UNSIGNED_BYTE, readback_.data());
    require_gl("paired rectification readback");
    const auto combined_stride =
        static_cast<std::size_t>(config_.output_width) * 2U;
    for (std::uint32_t row = 0; row < config_.output_height; ++row) {
      const auto* source = readback_.data() +
                           static_cast<std::size_t>(row) * combined_stride;
      std::memcpy(left_output.data +
                      static_cast<std::size_t>(row) * left_output.stride_bytes,
                  source, config_.output_width);
      std::memcpy(right_output.data +
                      static_cast<std::size_t>(row) * right_output.stride_bytes,
                  source + config_.output_width, config_.output_width);
    }

    stats = {};
    stats.compute_ms = timer_supported_
                           ? static_cast<double>(gpu_nanoseconds) / 1'000'000.0
                           : submit_wait_ms;
    stats.output_transfer_ms = milliseconds(output_started);
    stats.total_ms = milliseconds(total_started);
  }

  [[nodiscard]] std::size_t owned_fd_count() const noexcept override {
    return imports_.size();
  }

  [[nodiscard]] bool gpu_timer_supported() const noexcept override {
    return timer_supported_;
  }

 private:
  void initialize_egl() {
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display != nullptr) {
      display_ = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                      EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (display_ == EGL_NO_DISPLAY) {
      display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    require_egl(display_ != EGL_NO_DISPLAY, "eglGetDisplay");
    EGLint major = 0;
    EGLint minor = 0;
    require_egl(eglInitialize(display_, &major, &minor) == EGL_TRUE,
                "eglInitialize");
    require_egl(eglBindAPI(EGL_OPENGL_ES_API) == EGL_TRUE, "eglBindAPI");

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE,
    };
    EGLint count = 0;
    require_egl(eglChooseConfig(display_, config_attributes, &egl_config_, 1,
                                &count) == EGL_TRUE && count == 1,
                "eglChooseConfig");
    const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display_, egl_config_,
                                       surface_attributes);
    require_egl(surface_ != EGL_NO_SURFACE, "eglCreatePbufferSurface");
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                         EGL_NONE};
    context_ = eglCreateContext(display_, egl_config_, EGL_NO_CONTEXT,
                                context_attributes);
    require_egl(context_ != EGL_NO_CONTEXT, "eglCreateContext");
    make_current();

    const auto* renderer_bytes = reinterpret_cast<const char*>(
        glGetString(GL_RENDERER));
    const std::string renderer =
        renderer_bytes == nullptr ? std::string{} : std::string(renderer_bytes);
    if (renderer.find("V3D") == std::string::npos &&
        renderer.find("VideoCore") == std::string::npos) {
      throw std::runtime_error(
          "egl_gl_dmabuf requires the Raspberry Pi V3D renderer, got: " +
          (renderer.empty() ? std::string("unknown") : renderer));
    }

    const auto* egl_extensions = eglQueryString(display_, EGL_EXTENSIONS);
    if (!has_extension(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
      throw std::runtime_error("EGL_EXT_image_dma_buf_import is unavailable");
    }
    modifier_import_supported_ = has_extension(
        egl_extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    const auto* gl_extensions = reinterpret_cast<const char*>(
        glGetString(GL_EXTENSIONS));
    if (!has_extension(gl_extensions, "GL_OES_EGL_image")) {
      throw std::runtime_error("GL_OES_EGL_image is unavailable");
    }
    create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    image_target_texture_ =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (create_image_ == nullptr || destroy_image_ == nullptr ||
        image_target_texture_ == nullptr) {
      throw std::runtime_error("required EGLImage entry points are unavailable");
    }
  }

  void initialize_gl(const StereoRectificationMaps& maps) {
    program_ = make_program();
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_left_input"), 0);
    glUniform1i(glGetUniformLocation(program_, "u_right_input"), 1);
    glUniform1i(glGetUniformLocation(program_, "u_left_map"), 2);
    glUniform1i(glGetUniformLocation(program_, "u_right_map"), 3);
    glUniform2i(glGetUniformLocation(program_, "u_input_size"),
                static_cast<GLint>(config_.input_width),
                static_cast<GLint>(config_.input_height));
    glUniform1i(glGetUniformLocation(program_, "u_output_width"),
                static_cast<GLint>(config_.output_width));

    left_map_texture_ = upload_map(maps.left);
    right_map_texture_ = upload_map(maps.right);

    glGenTextures(1, &output_texture_);
    glBindTexture(GL_TEXTURE_2D, output_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
                 static_cast<GLsizei>(config_.output_width * 2U),
                 static_cast<GLsizei>(config_.output_height), 0, GL_RED,
                 GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &output_framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, output_framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, output_texture_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      throw std::runtime_error("rectification output framebuffer is incomplete");
    }

    const auto* extensions = reinterpret_cast<const char*>(
        glGetString(GL_EXTENSIONS));
    timer_supported_ = has_extension(extensions, "GL_EXT_disjoint_timer_query");
    if (timer_supported_) {
      gen_queries_ = reinterpret_cast<PFNGLGENQUERIESEXTPROC>(
          eglGetProcAddress("glGenQueriesEXT"));
      delete_queries_ = reinterpret_cast<PFNGLDELETEQUERIESEXTPROC>(
          eglGetProcAddress("glDeleteQueriesEXT"));
      begin_query_ = reinterpret_cast<PFNGLBEGINQUERYEXTPROC>(
          eglGetProcAddress("glBeginQueryEXT"));
      end_query_ = reinterpret_cast<PFNGLENDQUERYEXTPROC>(
          eglGetProcAddress("glEndQueryEXT"));
      get_query_ui64_ = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(
          eglGetProcAddress("glGetQueryObjectui64vEXT"));
      timer_supported_ = gen_queries_ != nullptr && delete_queries_ != nullptr &&
                         begin_query_ != nullptr && end_query_ != nullptr &&
                         get_query_ui64_ != nullptr;
      if (timer_supported_) {
        gen_queries_(1, &timer_query_);
      }
    }
    require_gl("OpenGL rectification initialization");
  }

  GLuint upload_map(const RectificationMap& map) {
    const auto packed = pack_quantized_map(map);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F,
                 static_cast<GLsizei>(map.width),
                 static_cast<GLsizei>(map.height), 0, GL_RG, GL_FLOAT,
                 packed.data());
    require_gl("rectification map upload");
    return texture;
  }

  void make_current() const {
    require_egl(eglMakeCurrent(display_, surface_, surface_, context_) ==
                    EGL_TRUE,
                "eglMakeCurrent");
  }

  [[nodiscard]] const ImportedResource* find_import(
      EglGlImportedImage handle) const {
    const auto found = std::find_if(
        imports_.begin(), imports_.end(),
        [handle](const ImportedResource& resource) {
          return resource.handle == handle;
        });
    return found == imports_.end() ? nullptr : &*found;
  }

  void shutdown() noexcept {
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
      static_cast<void>(eglMakeCurrent(display_, surface_, surface_, context_));
      while (!imports_.empty()) {
        release_import(imports_.back().handle);
      }
      if (timer_query_ != 0 && delete_queries_ != nullptr) {
        delete_queries_(1, &timer_query_);
      }
      if (output_framebuffer_ != 0) {
        glDeleteFramebuffers(1, &output_framebuffer_);
      }
      const std::array<GLuint, 3> textures = {
          left_map_texture_, right_map_texture_, output_texture_};
      glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
      if (program_ != 0) {
        glDeleteProgram(program_);
      }
      static_cast<void>(eglMakeCurrent(display_, EGL_NO_SURFACE,
                                       EGL_NO_SURFACE, EGL_NO_CONTEXT));
    } else {
      for (const auto& resource : imports_) {
        if (resource.owned_fd >= 0) {
          close(resource.owned_fd);
        }
      }
      imports_.clear();
    }
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
      static_cast<void>(eglDestroyContext(display_, context_));
    }
    if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
      static_cast<void>(eglDestroySurface(display_, surface_));
    }
    if (display_ != EGL_NO_DISPLAY) {
      static_cast<void>(eglTerminate(display_));
    }
    display_ = EGL_NO_DISPLAY;
    context_ = EGL_NO_CONTEXT;
    surface_ = EGL_NO_SURFACE;
  }

  RectificationBackendConfig config_;
  EGLDisplay display_{EGL_NO_DISPLAY};
  EGLConfig egl_config_{};
  EGLSurface surface_{EGL_NO_SURFACE};
  EGLContext context_{EGL_NO_CONTEXT};
  PFNEGLCREATEIMAGEKHRPROC create_image_{};
  PFNEGLDESTROYIMAGEKHRPROC destroy_image_{};
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_{};
  bool modifier_import_supported_{};
  GLuint program_{};
  GLuint left_map_texture_{};
  GLuint right_map_texture_{};
  GLuint output_texture_{};
  GLuint output_framebuffer_{};
  std::vector<std::uint8_t> readback_;
  std::vector<ImportedResource> imports_;
  EglGlImportedImage next_handle_{1};

  bool timer_supported_{};
  GLuint timer_query_{};
  PFNGLGENQUERIESEXTPROC gen_queries_{};
  PFNGLDELETEQUERIESEXTPROC delete_queries_{};
  PFNGLBEGINQUERYEXTPROC begin_query_{};
  PFNGLENDQUERYEXTPROC end_query_{};
  PFNGLGETQUERYOBJECTUI64VEXTPROC get_query_ui64_{};
};

std::unique_ptr<RectificationBackend> make_cpu_fallback(
    const RectificationBackendConfig& config,
    const StereoRectificationMaps& maps) {
  RectificationBackendRegistry registry;
  register_opencv_rectification_backend(registry);
  return registry.create(kOpenCvRectificationBackendName, {}, false, config,
                         maps);
}

}  // namespace

void register_egl_gl_dmabuf_rectification_backend(
    RectificationBackendRegistry& registry,
    EglGlDmabufRectificationOptions options) {
  registry.register_backend(
      std::string(kEglGlDmabufRectificationBackendName),
      [options](const RectificationBackendConfig& config,
                const StereoRectificationMaps& maps) {
        auto fallback = options.allow_cpu_fallback
                            ? make_cpu_fallback(config, maps)
                            : std::unique_ptr<RectificationBackend>{};
        try {
          auto platform = std::make_unique<MesaEglGlDmabufPlatform>(
              config, maps, options.import_cache_capacity);
          return make_egl_gl_dmabuf_rectification_backend(
              config, options, std::move(platform), std::move(fallback));
        } catch (const std::exception& error) {
          if (!options.allow_cpu_fallback) {
            throw;
          }
          return make_egl_gl_dmabuf_rectification_backend(
              config, options, {}, std::move(fallback),
              std::string("GPU rectification initialization failure: ") +
                  error.what());
        }
      });
}

}  // namespace depthd
