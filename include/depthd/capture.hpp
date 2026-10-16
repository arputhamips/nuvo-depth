#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "depthd/buffer.hpp"
#include "depthd/products.hpp"

namespace depthd {

inline constexpr std::uint32_t kExternalTriggerGuardUs = 250U;

struct CameraIdentity {
  // Platform-stable identity supplied by the capture stack. On libcamera this
  // is Camera::id(), not the camera's discovery-list index.
  std::string stable_id;
  std::string model;
  std::string location;
  // Index in the platform-stable ID ordering returned by enumerate().
  std::uint32_t discovery_index{};
};

struct CameraBinding {
  CameraRole role{CameraRole::kLogicalLeft};
  CameraIdentity camera{};
};

void validate(const CameraIdentity& identity);
// Driver probe completion order can change after reboot. Apply configured
// indices only after canonicalizing the physical camera IDs.
void canonicalize_camera_order(std::vector<CameraIdentity>& cameras);
// Product wiring is fixed: logical-left is the IMX296 on RP1 i2c@80000,
// logical-right is on i2c@88000. Never infer these roles from list indices.
[[nodiscard]] std::array<CameraBinding, 2> bind_pi5_fixed_csi_ports(
    const std::vector<CameraIdentity>& cameras);
void validate(const CameraBinding& binding);
void validate_stereo_bindings(const CameraBinding& left,
                              const CameraBinding& right);

struct CapturedFrame {
  CameraRole role{CameraRole::kLogicalLeft};
  std::string camera_stable_id;
  std::uint64_t capture_epoch{};
  std::uint64_t stream_sequence{};
  std::int64_t sensor_timestamp_ns{};
  // Per-request libcamera metadata, sampled at ISP completion. Zero means the
  // camera pipeline did not report that control for this frame.
  std::int32_t isp_exposure_time_us{};
  float isp_analogue_gain{};
  float isp_digital_gain{};
  CaptureClock sensor_clock{CaptureClock::kUnknown};
  PixelGrid grid{};
  std::string pixel_format;
  std::vector<CaptureBufferPlane> planes;
  CaptureBufferLease lease{};

  CapturedFrame() = default;
  CapturedFrame(const CapturedFrame&) = delete;
  CapturedFrame& operator=(const CapturedFrame&) = delete;
  CapturedFrame(CapturedFrame&&) noexcept = default;
  CapturedFrame& operator=(CapturedFrame&&) noexcept = default;
};

struct StereoPair {
  CapturedFrame left{};
  CapturedFrame right{};
  std::uint64_t capture_epoch{};
  std::uint64_t pair_sequence{};
  std::int64_t capture_timestamp_ns{};
  std::uint64_t sensor_delta_ns{};

  StereoPair() = default;
  StereoPair(const StereoPair&) = delete;
  StereoPair& operator=(const StereoPair&) = delete;
  StereoPair(StereoPair&&) noexcept = default;
  StereoPair& operator=(StereoPair&&) noexcept = default;
};

struct PairerConfig {
  std::uint64_t tolerance_ns{500'000U};
  std::size_t pending_capacity_per_camera{4U};
};

struct PairerStats {
  std::uint64_t left_submitted{};
  std::uint64_t right_submitted{};
  std::uint64_t paired{};
  std::uint64_t zero_timestamp_rejected{};
  std::uint64_t wrong_clock_rejected{};
  std::uint64_t identity_rejected{};
  std::uint64_t epoch_rejected{};
  std::uint64_t duplicate_sequence_rejected{};
  std::uint64_t nonmonotonic_rejected{};
  std::uint64_t stale_dropped{};
  std::uint64_t capacity_dropped{};
  std::uint64_t maximum_sensor_delta_ns{};
};

// Associates already-completed left/right frames. It never guesses across a
// capture restart: begin_epoch() clears pending state and returns a new epoch.
// Camera IDs are pinned on the first epoch and must remain stable thereafter.
class StereoPairer final {
 public:
  explicit StereoPairer(PairerConfig config = {});
  ~StereoPairer();

  StereoPairer(const StereoPairer&) = delete;
  StereoPairer& operator=(const StereoPairer&) = delete;

  // May be changed only between capture epochs. Camera bindings remain pinned
  // across reconfiguration and restart.
  void configure(PairerConfig config);
  [[nodiscard]] std::uint64_t begin_epoch(const CameraBinding& left,
                                          const CameraBinding& right);
  void end_epoch() noexcept;

  [[nodiscard]] std::optional<StereoPair> submit(CapturedFrame frame);
  [[nodiscard]] PairerStats stats() const noexcept;
  [[nodiscard]] std::uint64_t active_epoch() const noexcept;
  [[nodiscard]] bool running() const noexcept;

 private:
  PairerConfig config_{};
  mutable std::mutex mutex_{};
  CameraBinding left_binding_{};
  CameraBinding right_binding_{};
  bool bindings_pinned_{};
  bool running_{};
  std::uint64_t epoch_counter_{};
  std::uint64_t pair_sequence_{};
  std::array<std::uint64_t, 2> last_stream_sequence_{};
  std::array<std::int64_t, 2> last_timestamp_ns_{};
  std::deque<CapturedFrame> left_pending_{};
  std::deque<CapturedFrame> right_pending_{};
  PairerStats stats_{};
};

struct CaptureOpenOptions {
  // Legacy schema/reporting values. The Pi5 backend selects fixed CSI ports.
  std::uint32_t left_camera_index{0U};
  std::uint32_t right_camera_index{1U};
  std::uint32_t width{728U};
  std::uint32_t height{544U};
  std::string pixel_format{"YUV420"};
  std::uint32_t orientation_degrees{180U};
  std::uint32_t buffer_count{4U};
  double target_hz{10.0};
  std::uint64_t pair_tolerance_ns{500'000U};
  bool hardware_trigger_enabled{true};
  std::string trigger_driver_mode{"external_xtrig"};
  std::string gpiochip{"/dev/gpiochip0"};
  std::uint32_t gpio_line{17U};
  bool trigger_idle_high{true};
  std::uint32_t trigger_pulse_width_us{8'000U};
  std::uint32_t startup_priming_pulses{5U};
  std::uint32_t exposure_time_us{8'000U};
  double analogue_gain{2.0};
  std::array<double, 2> colour_gains{2.4771, 2.3425};
};

void validate(const CaptureOpenOptions& options);

struct CaptureControls {
  std::uint32_t exposure_time_us{8'000U};
  double analogue_gain{2.0};
  std::array<double, 2> colour_gains{2.4771, 2.3425};
};

void validate(const CaptureControls& controls);

struct NegotiatedCameraStream {
  CameraBinding binding{};
  PixelGrid grid{};
  std::string pixel_format;
  std::uint32_t stride_bytes{};
  std::uint32_t buffer_count{};
};

struct CaptureSessionInfo {
  std::uint64_t capture_epoch{};
  NegotiatedCameraStream left{};
  NegotiatedCameraStream right{};
  bool hardware_trigger_active{};
  std::string trigger_source;
};

struct CaptureRuntimeStats {
  std::string trigger_source;
  bool trigger_hardware_timed{};
  bool trigger_pulse_timing_measured{};
  std::uint64_t trigger_commanded_pulse_us{};
  std::uint64_t trigger_period_ns{};
  PairerStats pairer{};
  std::uint64_t callback_or_trigger_errors{};
  std::uint64_t frame_completion_errors{};
  std::uint64_t trigger_errors{};
  std::uint64_t trigger_attempts{};
  std::uint64_t trigger_deadline_overruns{};
  std::uint64_t trigger_last_pulse_us{};
  std::uint64_t trigger_min_pulse_us{};
  std::uint64_t trigger_max_pulse_us{};
  std::uint64_t trigger_pulse_guard_overruns{};
  std::uint64_t trigger_max_pulse_overshoot_us{};
  std::uint64_t ready_pair_replacements{};
  std::uint64_t request_requeue_failures{};
};

class CaptureSource {
 public:
  virtual ~CaptureSource() = default;

  [[nodiscard]] virtual std::vector<CameraIdentity> enumerate() = 0;
  [[nodiscard]] virtual CaptureSessionInfo start(
      const CaptureOpenOptions& options) = 0;
  [[nodiscard]] virtual std::optional<StereoPair> wait_for_pair(
      std::chrono::milliseconds timeout) = 0;
  // Applies one shared command to both cameras. With external XTR capture,
  // exposure_time_us also controls the active-low trigger duration.
  virtual void set_controls(const CaptureControls& controls) = 0;
  virtual void stop() noexcept = 0;
  [[nodiscard]] virtual CaptureRuntimeStats runtime_stats() const noexcept = 0;
  [[nodiscard]] virtual std::size_t outstanding_leases() const noexcept = 0;
};

// Returns null when the binary was built without the Linux/Pi libcamera
// adapter. This keeps pairing and lifecycle tests portable to development
// hosts and leaves room for a future Luckfox CaptureSource implementation.
[[nodiscard]] std::unique_ptr<CaptureSource>
make_pi5_libcamera_capture_source();
[[nodiscard]] std::string_view pi5_capture_backend_status() noexcept;

// Bridge from a paired capture event to the associated product identity used
// by rectification, disparity, confidence, validity, and depth.
[[nodiscard]] FrameSetIdentity make_frame_set_identity(
    const StereoPair& pair, const std::array<std::uint8_t, 16>& device_uuid,
    const std::array<std::uint8_t, 16>& run_uuid,
    std::uint64_t configuration_epoch);

}  // namespace depthd
