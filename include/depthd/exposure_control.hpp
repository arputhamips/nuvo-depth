#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "depthd/runtime.hpp"

namespace depthd::exposure {

enum class Mode : std::uint8_t { kManual = 0, kAuto = 1 };

struct Configuration {
  std::uint32_t schema_version{1U};
  Mode mode{Mode::kManual};
  std::uint32_t manual_exposure_us{8'000U};
  double manual_analogue_gain{2.0};
  double red_gain{2.4771};
  double blue_gain{2.3425};
  double auto_target_median{75.0};
  double auto_min_median{65.0};
  double auto_max_median{85.0};
  std::uint32_t auto_min_exposure_us{250U};
  std::uint32_t auto_max_exposure_us{8'000U};
  double auto_min_analogue_gain{1.0};
  double auto_max_analogue_gain{3.0};
  std::uint32_t auto_update_interval_frames{2U};
  std::uint32_t auto_settle_frames_after_update{4U};
};

struct Command {
  std::uint32_t exposure_us{8'000U};
  double analogue_gain{2.0};
  double red_gain{2.4771};
  double blue_gain{2.3425};
};

// Runtime cadence is deliberately separate from the persisted schema. This
// keeps existing exposure.json files/API clients compatible while ensuring an
// XTR low pulse always leaves a re-arm margin before the next trigger.
struct TimingLimits {
  std::uint32_t frame_period_us{100'000U};
  std::uint32_t trigger_guard_us{250U};
};

// Profile settings, separate from the existing live-control JSON schema.
// An absent ROI uses the central 60%; coordinates are [xmin,ymin,xmax,ymax].
struct MeteringConfiguration {
  std::optional<std::array<std::uint32_t, 4>> roi;
  std::uint32_t dark_threshold{16U};
  std::uint32_t clipping_threshold{245U};
  double max_clipped_fraction{0.005};
};

struct AppliedControls {
  std::int32_t left_exposure_us{};
  std::int32_t right_exposure_us{};
  double left_analogue_gain{};
  double right_analogue_gain{};
};

struct Stats {
  std::uint64_t frames_seen{};
  std::uint64_t observations{};
  std::uint64_t updates{};
  std::uint64_t configuration_reloads{};
  std::uint64_t configuration_errors{};
  std::optional<double> median;
  std::optional<double> left_median;
  std::optional<double> right_median;
  std::optional<double> median_difference;
  std::optional<double> dark_fraction;
  std::optional<double> clipped_fraction;
  std::optional<double> left_clipped_fraction;
  std::optional<double> right_clipped_fraction;
  double last_adjustment_factor{1.0};
  std::uint32_t settle_frames_remaining{};
  std::uint32_t confirmation_observations{};
  std::uint32_t frame_period_us{};
  std::uint32_t trigger_guard_us{};
  std::uint32_t safe_max_exposure_us{};
  bool at_lower_limit{};
  bool at_upper_limit{};
  bool applied_controls_tracked{};
  bool applied_controls_available{};
  bool applied_controls_match{};
  std::uint64_t applied_controls_wait_frames{};
  std::string state;
  std::string confirmation_direction;
  std::string last_reason;
  std::string last_error;
};

[[nodiscard]] std::string_view name(Mode mode) noexcept;
void validate(const Configuration& configuration);
[[nodiscard]] Configuration parse_configuration(std::string_view document);
[[nodiscard]] Configuration load_configuration(
    const std::filesystem::path& path,
    const Configuration& fallback = Configuration{});
[[nodiscard]] std::string configuration_json(
    const Configuration& configuration);

// Owns one shared exposure value for a stereo pair. In auto mode it samples
// both ISP luma planes and emits a single command for both cameras and the XTR
// pulse source. Invalid control-file reloads leave the last valid command live.
class Controller final {
 public:
  Controller(std::filesystem::path control_file, Configuration fallback,
             TimingLimits timing = {}, MeteringConfiguration metering = {});

  [[nodiscard]] std::optional<Command> refresh();
  // Call for every captured pair, before metering admission. Once enabled,
  // missing/mismatched metadata holds AE; it never silently assumes application.
  // This acknowledges reported sensor controls, not physical trigger edges.
  void update_applied_controls(const AppliedControls& controls) noexcept;
  // Check before mapping either DMA buffer: most frames need no CPU metering.
  [[nodiscard]] bool should_meter_next_frame() const noexcept;
  void skip_observation_frame();
  [[nodiscard]] std::optional<Command> observe(const ImageView& left,
                                                const ImageView& right);
  [[nodiscard]] const Configuration& configuration() const noexcept;
  [[nodiscard]] const Command& command() const noexcept;
  [[nodiscard]] const Stats& stats() const noexcept;

 private:
  [[nodiscard]] Command command_for(const Configuration& configuration) const;
  [[nodiscard]] Command split_exposure_value(double value,
                                              std::string reason);
  [[nodiscard]] std::uint32_t safe_max_exposure_us() const noexcept;
  [[nodiscard]] std::uint32_t effective_auto_min_exposure_us() const noexcept;
  [[nodiscard]] std::uint32_t effective_auto_max_exposure_us() const noexcept;
  void reset_confirmation() noexcept;
  [[nodiscard]] bool confirm(const std::string& direction);
  void hold(const std::string& reason, double median);
  void update_state_stats() noexcept;

  std::filesystem::path control_file_;
  Configuration fallback_;
  Configuration configuration_;
  TimingLimits timing_;
  MeteringConfiguration metering_;
  Command command_;
  Stats stats_;
  std::optional<std::filesystem::file_time_type> last_write_time_;
  std::string last_document_;
  std::uint32_t settle_frames_remaining_{};
  std::uint32_t confirmation_observations_{};
  std::string confirmation_direction_;
  std::optional<double> filtered_log_median_;
  std::optional<double> stable_median_;
  bool protect_highlights_{};
};

}  // namespace depthd::exposure
