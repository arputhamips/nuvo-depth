#include "depthd/egl_gl_dmabuf_rectification_backend.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace {

void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

class Lease final : public depthd::DmabufRequestLease {
 public:
  bool active{true};
  std::uint64_t id{1};
  [[nodiscard]] bool valid() const noexcept override { return active; }
  [[nodiscard]] std::uint64_t request_id() const noexcept override {
    return id;
  }
};

struct PlatformState {
  std::uint64_t next_handle{1};
  std::uint64_t imports{};
  std::uint64_t releases{};
  std::uint64_t rectifies{};
  bool fail_rectify{};
  std::unordered_set<std::uint64_t> live;
};

class FakePlatform final : public depthd::EglGlDmabufPlatform {
 public:
  explicit FakePlatform(std::shared_ptr<PlatformState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] depthd::DmabufFdIdentity fd_identity(
      int fd) const override {
    return {7, static_cast<std::uint64_t>(fd + 1000)};
  }

  [[nodiscard]] depthd::EglGlImportedImage import_plane(
      const depthd::DmabufPlaneView&) override {
    const auto handle = state_->next_handle++;
    state_->live.insert(handle);
    ++state_->imports;
    return handle;
  }

  void release_import(depthd::EglGlImportedImage image) noexcept override {
    if (state_->live.erase(image) > 0) {
      ++state_->releases;
    }
  }

  void rectify_pair(depthd::EglGlImportedImage left,
                    depthd::EglGlImportedImage right,
                    const depthd::MutableImageView& left_output,
                    const depthd::MutableImageView& right_output,
                    depthd::RectificationStats& stats,
                    double& submit_wait_ms) override {
    ++state_->rectifies;
    if (state_->fail_rectify) {
      throw std::runtime_error("injected GL failure");
    }
    for (std::uint32_t row = 0; row < left_output.height; ++row) {
      std::fill_n(left_output.data + row * left_output.stride_bytes,
                  left_output.width, static_cast<std::uint8_t>(left));
      std::fill_n(right_output.data + row * right_output.stride_bytes,
                  right_output.width, static_cast<std::uint8_t>(right));
    }
    stats = {};
    stats.compute_ms = 1.25;
    stats.output_transfer_ms = 0.5;
    submit_wait_ms = 1.5;
  }

  [[nodiscard]] std::size_t owned_fd_count() const noexcept override {
    return state_->live.size();
  }

  [[nodiscard]] bool gpu_timer_supported() const noexcept override {
    return true;
  }

 private:
  std::shared_ptr<PlatformState> state_;
};

struct CpuState {
  std::uint64_t calls{};
};

class FakeCpu final : public depthd::RectificationBackend {
 public:
  explicit FakeCpu(std::shared_ptr<CpuState> state)
      : state_(std::move(state)) {}
  [[nodiscard]] std::string_view name() const noexcept override {
    return "cpu_opencv";
  }
  void rectify(depthd::RectificationCamera, const depthd::ImageView&,
               const depthd::MutableImageView& output,
               depthd::RectificationStats& stats) override {
    ++state_->calls;
    for (std::uint32_t row = 0; row < output.height; ++row) {
      std::fill_n(output.data + row * output.stride_bytes, output.width,
                  static_cast<std::uint8_t>(9));
    }
    stats = {};
    stats.compute_ms = 2.0;
    stats.total_ms = 2.0;
  }

 private:
  std::shared_ptr<CpuState> state_;
};

depthd::DmabufPlaneView plane(int fd, std::uint64_t buffer_id,
                             Lease& lease) {
  return depthd::DmabufPlaneView{
      fd, buffer_id, 4, 3, 4, 0, depthd::kDmabufModifierInvalid,
      depthd::DmabufPlaneFormat::kR8, &lease};
}

}  // namespace

int main() {
  depthd::RectificationBackendConfig config;
  config.input_width = 4;
  config.input_height = 3;
  config.output_width = 4;
  config.output_height = 3;
  depthd::EglGlDmabufRectificationOptions options;
  options.import_cache_capacity = 2;
  options.allow_cpu_fallback = false;

  Lease left_lease;
  Lease right_lease;
  right_lease.id = 2;
  depthd::DmabufStereoInput input;
  input.left = plane(10, 100, left_lease);
  input.right = plane(11, 200, right_lease);
  std::array<std::uint8_t, 18> left_output{};
  std::array<std::uint8_t, 18> right_output{};
  const depthd::MutableImageView left_view{
      left_output.data(), 4, 3, 6, depthd::ImageEncoding::kGray8};
  const depthd::MutableImageView right_view{
      right_output.data(), 4, 3, 6, depthd::ImageEncoding::kGray8};

  auto state = std::make_shared<PlatformState>();
  auto backend = depthd::make_egl_gl_dmabuf_rectification_backend(
      config, options, std::make_unique<FakePlatform>(state), {}, {});
  expect(backend->name() == "egl_gl_dmabuf" &&
             backend->supports_dmabuf_pair(),
         "backend identity/capability mismatch");
  depthd::RectificationStats stats;
  backend->rectify_dmabuf_pair(input, left_view, right_view, stats);
  expect(state->imports == 2 && state->live.size() == 2 &&
             state->rectifies == 1,
         "initial pair was not imported exactly once");
  expect(left_output[0] == 1 && right_output[0] == 2 &&
             stats.compute_ms == 1.25,
         "fake paired result/timing mismatch");

  backend->rectify_dmabuf_pair(input, left_view, right_view, stats);
  expect(state->imports == 2 && state->rectifies == 2,
         "cache hit unexpectedly re-imported a buffer");

  input.left.stride_bytes = 5;
  backend->rectify_dmabuf_pair(input, left_view, right_view, stats);
  auto diagnostics = backend->dmabuf_diagnostics();
  expect(state->imports == 3 && state->releases == 1 &&
             diagnostics.stale_reimport_count == 1,
         "layout change did not force a stale re-import");

  input.left = plane(12, 300, left_lease);
  input.right = plane(13, 400, right_lease);
  backend->rectify_dmabuf_pair(input, left_view, right_view, stats);
  diagnostics = backend->dmabuf_diagnostics();
  expect(diagnostics.cache_size == 2 && diagnostics.eviction_count == 2 &&
             diagnostics.owned_fd_count == 2,
         "bounded cache did not evict least-recent imports");

  left_lease.active = false;
  try {
    backend->rectify_dmabuf_pair(input, left_view, right_view, stats);
    throw std::runtime_error("expired lease was accepted");
  } catch (const std::invalid_argument&) {
  }
  left_lease.active = true;
  backend.reset();
  expect(state->live.empty() && state->releases == state->imports,
         "destruction leaked fake imports");

  auto failing_state = std::make_shared<PlatformState>();
  failing_state->fail_rectify = true;
  auto cpu_state = std::make_shared<CpuState>();
  options.allow_cpu_fallback = true;
  auto fallback_backend =
      depthd::make_egl_gl_dmabuf_rectification_backend(
          config, options, std::make_unique<FakePlatform>(failing_state),
          std::make_unique<FakeCpu>(cpu_state), {});
  input.left = plane(20, 500, left_lease);
  input.right = plane(21, 600, right_lease);
  std::array<std::uint8_t, 12> cpu_left{};
  std::array<std::uint8_t, 12> cpu_right{};
  input.left_cpu_fallback = {cpu_left.data(), 4, 3, 4,
                             depthd::ImageEncoding::kGray8};
  input.right_cpu_fallback = {cpu_right.data(), 4, 3, 4,
                              depthd::ImageEncoding::kGray8};
  fallback_backend->rectify_dmabuf_pair(input, left_view, right_view, stats);
  diagnostics = fallback_backend->dmabuf_diagnostics();
  expect(diagnostics.selected_backend == "cpu_opencv" &&
             diagnostics.failure_count == 1 &&
             diagnostics.fallback_reason.find("injected GL failure") !=
                 std::string::npos &&
             diagnostics.cache_size == 0 &&
             failing_state->live.empty() && cpu_state->calls == 2 &&
             left_output[0] == 9 && right_output[0] == 9,
         "runtime fallback did not release/import/report correctly");

  auto init_cpu_state = std::make_shared<CpuState>();
  auto init_fallback = depthd::make_egl_gl_dmabuf_rectification_backend(
      config, options, {}, std::make_unique<FakeCpu>(init_cpu_state),
      "injected initialization failure");
  init_fallback->rectify_dmabuf_pair(input, left_view, right_view, stats);
  expect(init_fallback->dmabuf_diagnostics().fallback_reason ==
             "injected initialization failure" &&
             init_cpu_state->calls == 2,
         "initialization fallback failed");

  options.allow_cpu_fallback = false;
  try {
    static_cast<void>(depthd::make_egl_gl_dmabuf_rectification_backend(
        config, options, {}, {}, "required EGL extension missing"));
    throw std::runtime_error("disabled initialization fallback was accepted");
  } catch (const std::runtime_error& error) {
    expect(std::string_view(error.what()).find("required EGL") !=
               std::string_view::npos,
           "initialization error lost its reason");
  }

  depthd::RectificationBackendRegistry registry;
  registry.register_backend(
      std::string(depthd::kEglGlDmabufRectificationBackendName),
      [](const depthd::RectificationBackendConfig& local_config,
         const depthd::StereoRectificationMaps&) {
        depthd::EglGlDmabufRectificationOptions local_options;
        local_options.import_cache_capacity = 2;
        local_options.allow_cpu_fallback = false;
        return depthd::make_egl_gl_dmabuf_rectification_backend(
            local_config, local_options,
            std::make_unique<FakePlatform>(
                std::make_shared<PlatformState>()),
            {}, {});
      });
  expect(registry.contains("egl_gl_dmabuf"),
         "exact registry name was not accepted");
  return 0;
}
