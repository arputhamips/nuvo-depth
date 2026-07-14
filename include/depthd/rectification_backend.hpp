#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "depthd/stereo_backend.hpp"

namespace depthd {

struct MutableImageView {
  std::uint8_t* data{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t stride_bytes{};
  ImageEncoding encoding{ImageEncoding::kGray8};
};

// Dense inverse-remap coordinates in source-image pixel space. Each output
// pixel samples source_x[index], source_y[index]. The maps are generated once
// from the calibrated camera model and are immutable during steady state.
struct RectificationMap {
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<float> source_x;
  std::vector<float> source_y;
};

struct StereoRectificationMaps {
  RectificationMap left;
  RectificationMap right;
};

enum class RectificationCamera : std::uint8_t {
  kLeft,
  kRight,
};

enum class RectificationInterpolation : std::uint8_t {
  kLinear,
};

struct RectificationBackendConfig {
  std::uint32_t input_width{728};
  std::uint32_t input_height{544};
  std::uint32_t output_width{728};
  std::uint32_t output_height{544};
  RectificationInterpolation interpolation{RectificationInterpolation::kLinear};
  std::uint8_t border_value{0};
};

struct RectificationStats {
  double input_transfer_ms{};
  double compute_ms{};
  double output_transfer_ms{};
  double total_ms{};
};

// Capture-native luma input. This is deliberately separate from ImageView:
// an fd is an importable allocation, not a CPU pointer. The caller must retain
// the associated camera request lease for the complete synchronous call.
inline constexpr std::uint64_t kDmabufModifierInvalid =
    UINT64_MAX;

enum class DmabufPlaneFormat : std::uint8_t {
  kR8,
};

class DmabufRequestLease {
 public:
  virtual ~DmabufRequestLease() = default;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t request_id() const noexcept = 0;
};

struct DmabufPlaneView {
  int fd{-1};
  // Stable identity assigned by the capture adapter for one libcamera pool
  // allocation. fd alone is insufficient because descriptor numbers can be
  // reused after a camera restart.
  std::uint64_t buffer_id{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t stride_bytes{};
  std::uint32_t offset_bytes{};
  std::uint64_t modifier{kDmabufModifierInvalid};
  DmabufPlaneFormat format{DmabufPlaneFormat::kR8};
  const DmabufRequestLease* lease{};
};

struct DmabufStereoInput {
  DmabufPlaneView left;
  DmabufPlaneView right;

  // Optional zero-copy CPU mappings of the same leased planes. They may be
  // null on the fast path. A backend uses them only after a configured GPU
  // initialization/import/runtime failure.
  ImageView left_cpu_fallback;
  ImageView right_cpu_fallback;
};

struct DmabufRectificationDiagnostics {
  std::string selected_backend;
  std::string fallback_reason;
  std::size_t cache_size{};
  std::size_t cache_capacity{};
  std::uint64_t import_count{};
  std::uint64_t stale_reimport_count{};
  std::uint64_t eviction_count{};
  std::uint64_t failure_count{};
  std::size_t owned_fd_count{};
  double submit_wait_ms{};
  bool gpu_timer_supported{};
  RectificationStats last_stats;
};

class RectificationBackend {
 public:
  virtual ~RectificationBackend() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  virtual void rectify(RectificationCamera camera, const ImageView& input,
                       const MutableImageView& output,
                       RectificationStats& stats) = 0;

  // Backends may override this to batch both cameras in one submission. The
  // default implementation calls rectify twice and combines the measurements.
  virtual void rectify_pair(const ImageView& left_input,
                            const ImageView& right_input,
                            const MutableImageView& left_output,
                            const MutableImageView& right_output,
                            RectificationStats& stats);

  [[nodiscard]] virtual bool supports_dmabuf_pair() const noexcept {
    return false;
  }

  virtual void rectify_dmabuf_pair(
      const DmabufStereoInput& input,
      const MutableImageView& left_output,
      const MutableImageView& right_output,
      RectificationStats& stats);

  [[nodiscard]] virtual DmabufRectificationDiagnostics
  dmabuf_diagnostics() const;
};

using RectificationBackendFactory = std::function<
    std::unique_ptr<RectificationBackend>(const RectificationBackendConfig&,
                                          const StereoRectificationMaps&)>;

// Runtime registry keeps optional implementations out of the common contract.
// A daemon registers only the modules compiled into its process, then selects
// the requested backend by configuration with an explicit fallback policy.
class RectificationBackendRegistry {
 public:
  void register_backend(std::string name,
                        RectificationBackendFactory factory);

  [[nodiscard]] bool contains(std::string_view name) const;

  [[nodiscard]] std::unique_ptr<RectificationBackend> create(
      std::string_view requested_backend, std::string_view fallback_backend,
      bool allow_fallback, const RectificationBackendConfig& config,
      const StereoRectificationMaps& maps) const;

 private:
  std::unordered_map<std::string, RectificationBackendFactory> factories_;
};

[[nodiscard]] bool is_valid(const RectificationMap& map) noexcept;
[[nodiscard]] bool is_valid(const StereoRectificationMaps& maps) noexcept;

}  // namespace depthd
