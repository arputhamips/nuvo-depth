#include "depthd/exposure_control.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace depthd::exposure {
namespace {

constexpr std::uint32_t kAbsoluteMinimumExposureUs = 250U;
constexpr std::uint32_t kAbsoluteMaximumExposureUs = 8'000U;
constexpr double kAbsoluteMinimumGain = 1.0;
constexpr double kAbsoluteMaximumGain = 3.0;
constexpr std::uint32_t kConfirmationObservations = 3U;
constexpr double kMaximumStepEv = 0.2;

template <class Value>
[[nodiscard]] Value required(const YAML::Node& root, const char* key) {
  const auto node = root[key];
  if (!node || !node.IsScalar()) {
    throw std::invalid_argument(std::string("exposure.") + key +
                                " is required");
  }
  try {
    return node.as<Value>();
  } catch (const YAML::Exception&) {
    throw std::invalid_argument(std::string("exposure.") + key +
                                " has the wrong type");
  }
}

[[nodiscard]] Mode parse_mode(const std::string_view value) {
  if (value == "manual") return Mode::kManual;
  if (value == "auto") return Mode::kAuto;
  throw std::invalid_argument("exposure.mode must be manual or auto");
}

[[nodiscard]] bool finite_in_range(const double value, const double minimum,
                                   const double maximum) {
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] double quantize_gain(const double value, const double minimum,
                                   const double maximum) {
  return std::clamp(std::round(value / 0.05) * 0.05, minimum, maximum);
}

[[nodiscard]] std::uint32_t quantize_exposure(
    const double value, const std::uint32_t minimum,
    const std::uint32_t maximum) {
  const auto quantized = std::round(value / 50.0) * 50.0;
  return static_cast<std::uint32_t>(
      std::clamp(quantized, static_cast<double>(minimum),
                 static_cast<double>(maximum)));
}

struct Observation {
  double median{};
  double left_median{};
  double right_median{};
  double dark_fraction{};
  double clipped_fraction{};
  double left_clipped_fraction{};
  double right_clipped_fraction{};
};

[[nodiscard]] Observation measure(const ImageView& left,
                                  const ImageView& right,
                                  const MeteringConfiguration& configuration) {
  if (left.data == nullptr || right.data == nullptr || left.width == 0U ||
      left.height == 0U || right.width == 0U || right.height == 0U ||
      left.stride_bytes < left.width || right.stride_bytes < right.width ||
      left.encoding != ImageEncoding::kGray8 ||
      right.encoding != ImageEncoding::kGray8 || left.width != right.width ||
      left.height != right.height) {
    throw std::invalid_argument(
        "auto exposure requires equal uint8 stereo luma images");
  }
  const auto roi = configuration.roi.value_or(
      std::array<std::uint32_t, 4>{left.width / 5U, left.height / 5U,
                                  left.width * 4U / 5U, left.height * 4U / 5U});
  const auto [x_begin, y_begin, x_end, y_end] = roi;
  if (x_begin >= x_end || y_begin >= y_end || x_end > left.width ||
      y_end > left.height) {
    throw std::invalid_argument("auto-exposure metering ROI is outside the image");
  }
  std::array<std::array<std::size_t, 256>, 2> histograms{};
  std::array<std::size_t, 2> counts{};
  std::size_t dark{};
  std::array<std::size_t, 2> clipped{};
  const auto sample = [&](const ImageView& image, const std::size_t side) {
    for (auto y = y_begin; y < y_end; y += 4U) {
      const auto* row = image.data + static_cast<std::size_t>(y) *
                                         image.stride_bytes;
      for (auto x = x_begin; x < x_end; x += 4U) {
        const auto value = row[x];
        ++histograms[side][value];
        ++counts[side];
        if (value < configuration.dark_threshold) ++dark;
        if (value > configuration.clipping_threshold) ++clipped[side];
      }
    }
  };
  sample(left, 0U);
  sample(right, 1U);
  const auto median = [](const std::array<std::size_t, 256>& histogram,
                         const std::size_t count) {
    std::size_t cumulative{};
    for (std::size_t value = 0; value < histogram.size(); ++value) {
      cumulative += histogram[value];
      if (cumulative > count / 2U) return static_cast<double>(value);
    }
    return 255.0;
  };
  std::array<std::size_t, 256> combined{};
  for (std::size_t value = 0; value < combined.size(); ++value) {
    combined[value] = histograms[0][value] + histograms[1][value];
  }
  Observation result;
  result.median = median(combined, counts[0] + counts[1]);
  result.left_median = median(histograms[0], counts[0]);
  result.right_median = median(histograms[1], counts[1]);
  result.dark_fraction = static_cast<double>(dark) /
                         static_cast<double>(counts[0] + counts[1]);
  result.left_clipped_fraction =
      static_cast<double>(clipped[0]) /
      static_cast<double>(counts[0]);
  result.right_clipped_fraction =
      static_cast<double>(clipped[1]) /
      static_cast<double>(counts[1]);
  result.clipped_fraction =
      std::max(result.left_clipped_fraction, result.right_clipped_fraction);
  return result;
}

}  // namespace

std::string_view name(const Mode mode) noexcept {
  return mode == Mode::kAuto ? "auto" : "manual";
}

void validate(const Configuration& value) {
  if (value.schema_version != 1U) {
    throw std::invalid_argument("exposure.schema_version must be 1");
  }
  if (value.manual_exposure_us < kAbsoluteMinimumExposureUs ||
      value.manual_exposure_us > kAbsoluteMaximumExposureUs) {
    throw std::invalid_argument(
        "manual exposure must be between 250 and 8000 microseconds");
  }
  if (!finite_in_range(value.manual_analogue_gain, kAbsoluteMinimumGain,
                       kAbsoluteMaximumGain)) {
    throw std::invalid_argument("manual analogue gain must be between 1x and 3x");
  }
  if (!finite_in_range(value.red_gain, 0.1, 16.0) ||
      !finite_in_range(value.blue_gain, 0.1, 16.0)) {
    throw std::invalid_argument("colour gains must be between 0.1x and 16x");
  }
  if (!finite_in_range(value.auto_target_median, 1.0, 254.0) ||
      !finite_in_range(value.auto_min_median, 1.0, 254.0) ||
      !finite_in_range(value.auto_max_median, 1.0, 254.0) ||
      value.auto_min_median > value.auto_target_median ||
      value.auto_target_median > value.auto_max_median) {
    throw std::invalid_argument("auto-exposure median target/deadband is invalid");
  }
  if (value.auto_min_exposure_us < kAbsoluteMinimumExposureUs ||
      value.auto_max_exposure_us > kAbsoluteMaximumExposureUs ||
      value.auto_min_exposure_us > value.auto_max_exposure_us) {
    throw std::invalid_argument("auto-exposure shutter range is invalid");
  }
  if (!finite_in_range(value.auto_min_analogue_gain, kAbsoluteMinimumGain,
                       kAbsoluteMaximumGain) ||
      !finite_in_range(value.auto_max_analogue_gain, kAbsoluteMinimumGain,
                       kAbsoluteMaximumGain) ||
      value.auto_min_analogue_gain > value.auto_max_analogue_gain) {
    throw std::invalid_argument("auto-exposure gain range is invalid");
  }
  if (value.auto_update_interval_frames < 1U ||
      value.auto_update_interval_frames > 120U ||
      value.auto_settle_frames_after_update > 120U) {
    throw std::invalid_argument("auto-exposure update/settle cadence is invalid");
  }
}

Configuration parse_configuration(const std::string_view document) {
  YAML::Node root;
  try {
    root = YAML::Load(std::string(document));
  } catch (const YAML::Exception& error) {
    throw std::invalid_argument(std::string("invalid exposure JSON: ") +
                                error.what());
  }
  if (!root.IsMap()) {
    throw std::invalid_argument("exposure configuration must be an object");
  }
  constexpr std::array<std::string_view, 15> keys{
      "schema_version", "mode", "manual_exposure_us",
      "manual_analogue_gain", "red_gain", "blue_gain",
      "auto_target_median", "auto_min_median", "auto_max_median",
      "auto_min_exposure_us", "auto_max_exposure_us",
      "auto_min_analogue_gain", "auto_max_analogue_gain",
      "auto_update_interval_frames", "auto_settle_frames_after_update"};
  if (root.size() != keys.size()) {
    throw std::invalid_argument(
        "exposure configuration fields are incomplete or unknown");
  }
  for (const auto& entry : root) {
    const auto key = entry.first.as<std::string>();
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
      throw std::invalid_argument("unknown exposure field: " + key);
    }
  }
  Configuration result;
  result.schema_version = required<std::uint32_t>(root, "schema_version");
  result.mode = parse_mode(required<std::string>(root, "mode"));
  result.manual_exposure_us =
      required<std::uint32_t>(root, "manual_exposure_us");
  result.manual_analogue_gain =
      required<double>(root, "manual_analogue_gain");
  result.red_gain = required<double>(root, "red_gain");
  result.blue_gain = required<double>(root, "blue_gain");
  result.auto_target_median = required<double>(root, "auto_target_median");
  result.auto_min_median = required<double>(root, "auto_min_median");
  result.auto_max_median = required<double>(root, "auto_max_median");
  result.auto_min_exposure_us =
      required<std::uint32_t>(root, "auto_min_exposure_us");
  result.auto_max_exposure_us =
      required<std::uint32_t>(root, "auto_max_exposure_us");
  result.auto_min_analogue_gain =
      required<double>(root, "auto_min_analogue_gain");
  result.auto_max_analogue_gain =
      required<double>(root, "auto_max_analogue_gain");
  result.auto_update_interval_frames =
      required<std::uint32_t>(root, "auto_update_interval_frames");
  result.auto_settle_frames_after_update =
      required<std::uint32_t>(root, "auto_settle_frames_after_update");
  validate(result);
  return result;
}

Configuration load_configuration(const std::filesystem::path& path,
                                 const Configuration& fallback) {
  std::ifstream input(path);
  if (!input) return fallback;
  return parse_configuration(
      std::string(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>()));
}

std::string configuration_json(const Configuration& value) {
  validate(value);
  std::ostringstream output;
  output << "{\"schema_version\":1,\"mode\":\"" << name(value.mode)
         << "\",\"manual_exposure_us\":" << value.manual_exposure_us
         << ",\"manual_analogue_gain\":" << value.manual_analogue_gain
         << ",\"red_gain\":" << value.red_gain
         << ",\"blue_gain\":" << value.blue_gain
         << ",\"auto_target_median\":" << value.auto_target_median
         << ",\"auto_min_median\":" << value.auto_min_median
         << ",\"auto_max_median\":" << value.auto_max_median
         << ",\"auto_min_exposure_us\":" << value.auto_min_exposure_us
         << ",\"auto_max_exposure_us\":" << value.auto_max_exposure_us
         << ",\"auto_min_analogue_gain\":"
         << value.auto_min_analogue_gain
         << ",\"auto_max_analogue_gain\":"
         << value.auto_max_analogue_gain
         << ",\"auto_update_interval_frames\":"
         << value.auto_update_interval_frames
         << ",\"auto_settle_frames_after_update\":"
         << value.auto_settle_frames_after_update << '}';
  return output.str();
}

Controller::Controller(std::filesystem::path control_file,
                       Configuration fallback, const TimingLimits timing,
                       MeteringConfiguration metering)
    : control_file_(std::move(control_file)), fallback_(fallback),
      configuration_(fallback), timing_(timing), metering_(std::move(metering)) {
  validate(fallback_);
  if (metering_.dark_threshold >= metering_.clipping_threshold ||
      metering_.clipping_threshold > 255U ||
      !finite_in_range(metering_.max_clipped_fraction, 0.0, 1.0) ||
      (metering_.roi && ((*metering_.roi)[0] >= (*metering_.roi)[2] ||
                         (*metering_.roi)[1] >= (*metering_.roi)[3]))) {
    throw std::invalid_argument("auto-exposure metering settings are invalid");
  }
  if (timing_.trigger_guard_us == 0U ||
      timing_.frame_period_us <=
          kAbsoluteMinimumExposureUs + timing_.trigger_guard_us) {
    throw std::invalid_argument(
        "exposure timing must leave a non-zero trigger guard");
  }
  command_ = command_for(fallback_);
  try {
    std::ifstream input(control_file_);
    if (input) {
      last_document_.assign(std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>());
      configuration_ = parse_configuration(last_document_);
    }
    command_ = command_for(configuration_);
    if (std::filesystem::exists(control_file_)) {
      last_write_time_ = std::filesystem::last_write_time(control_file_);
    }
  } catch (const std::exception& error) {
    ++stats_.configuration_errors;
    stats_.last_error = error.what();
  }
  stats_.state = configuration_.mode == Mode::kAuto ? "auto_ready" : "manual";
  stats_.last_reason = "startup";
  update_state_stats();
}

std::uint32_t Controller::safe_max_exposure_us() const noexcept {
  return std::min(kAbsoluteMaximumExposureUs,
                  timing_.frame_period_us - timing_.trigger_guard_us);
}

std::uint32_t Controller::effective_auto_min_exposure_us() const noexcept {
  return std::min(configuration_.auto_min_exposure_us,
                  safe_max_exposure_us());
}

std::uint32_t Controller::effective_auto_max_exposure_us() const noexcept {
  return std::max(effective_auto_min_exposure_us(),
                  std::min(configuration_.auto_max_exposure_us,
                           safe_max_exposure_us()));
}

Command Controller::command_for(const Configuration& value) const {
  const auto safe_max = safe_max_exposure_us();
  const auto auto_min = std::min(value.auto_min_exposure_us, safe_max);
  const auto auto_max =
      std::max(auto_min, std::min(value.auto_max_exposure_us, safe_max));
  Command result;
  result.exposure_us = value.mode == Mode::kManual
                           ? std::min(value.manual_exposure_us, safe_max)
                           : std::clamp(value.manual_exposure_us,
                                        auto_min, auto_max);
  result.analogue_gain = value.mode == Mode::kManual
                             ? value.manual_analogue_gain
                             : std::clamp(value.manual_analogue_gain,
                                          value.auto_min_analogue_gain,
                                          value.auto_max_analogue_gain);
  result.red_gain = value.red_gain;
  result.blue_gain = value.blue_gain;
  return result;
}

std::optional<Command> Controller::refresh() {
  try {
    if (!std::filesystem::exists(control_file_)) return std::nullopt;
    const auto modified = std::filesystem::last_write_time(control_file_);
    if (last_write_time_ == modified) return std::nullopt;
    std::ifstream input(control_file_);
    if (!input) throw std::runtime_error("cannot read exposure control file");
    const std::string document{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    if (document == last_document_) {
      last_write_time_ = modified;
      return std::nullopt;
    }
    // Remember invalid documents too so a bad atomic update is reported once,
    // while the last valid command remains active until the file changes.
    last_document_ = document;
    last_write_time_ = modified;
    auto loaded = parse_configuration(document);
    const auto previous_mode = configuration_.mode;
    Command updated;
    if (loaded.mode == Mode::kManual) {
      updated = command_for(loaded);
    } else {
      // Entering auto, or editing its limits while it is active, starts from
      // the live shared command. This avoids a visible jump back to stale
      // manual seed fields before the first metering observation.
      const auto safe_max = safe_max_exposure_us();
      const auto minimum = std::min(loaded.auto_min_exposure_us, safe_max);
      const auto maximum =
          std::max(minimum, std::min(loaded.auto_max_exposure_us, safe_max));
      updated = command_;
      updated.exposure_us =
          std::clamp(updated.exposure_us, minimum, maximum);
      updated.analogue_gain =
          std::clamp(updated.analogue_gain, loaded.auto_min_analogue_gain,
                     loaded.auto_max_analogue_gain);
      updated.red_gain = loaded.red_gain;
      updated.blue_gain = loaded.blue_gain;
    }
    configuration_ = loaded;
    command_ = updated;
    last_write_time_ = modified;
    settle_frames_remaining_ = configuration_.mode == Mode::kAuto
                                   ? configuration_.auto_settle_frames_after_update
                                   : 0U;
    reset_confirmation();
    filtered_log_median_.reset();
    stable_median_.reset();
    protect_highlights_ = false;
    stats_.applied_controls_match = false;
    ++stats_.configuration_reloads;
    stats_.last_error.clear();
    stats_.last_reason =
        configuration_.mode == Mode::kAuto && previous_mode == Mode::kManual
            ? "configuration_auto_continuity"
            : "configuration";
    stats_.state = configuration_.mode == Mode::kAuto ? "settling" : "manual";
    update_state_stats();
    return command_;
  } catch (const std::exception& error) {
    ++stats_.configuration_errors;
    stats_.last_error = error.what();
    return std::nullopt;
  }
}

Command Controller::split_exposure_value(const double requested,
                                         std::string reason) {
  const auto& value = configuration_;
  const auto minimum_exposure = effective_auto_min_exposure_us();
  const auto maximum_exposure = effective_auto_max_exposure_us();
  const auto minimum = static_cast<double>(minimum_exposure) *
                       value.auto_min_analogue_gain;
  const auto maximum = static_cast<double>(maximum_exposure) *
                       value.auto_max_analogue_gain;
  const auto exposure_value = std::clamp(requested, minimum, maximum);
  Command result = command_;
  if (exposure_value <= static_cast<double>(maximum_exposure) *
                            value.auto_min_analogue_gain) {
    result.analogue_gain = value.auto_min_analogue_gain;
    result.exposure_us = quantize_exposure(
        exposure_value / result.analogue_gain, minimum_exposure,
        maximum_exposure);
  } else {
    result.exposure_us = maximum_exposure;
    result.analogue_gain = quantize_gain(
        exposure_value / static_cast<double>(result.exposure_us),
        value.auto_min_analogue_gain, value.auto_max_analogue_gain);
  }
  stats_.last_reason = std::move(reason);
  return result;
}

void Controller::reset_confirmation() noexcept {
  confirmation_observations_ = 0U;
  confirmation_direction_.clear();
}

bool Controller::confirm(const std::string& direction) {
  if (confirmation_direction_ == direction) {
    ++confirmation_observations_;
  } else {
    confirmation_direction_ = direction;
    confirmation_observations_ = 1U;
  }
  if (confirmation_observations_ >= kConfirmationObservations) return true;
  stats_.state = "confirming_" + direction;
  stats_.last_reason = stats_.state;
  update_state_stats();
  return false;
}

void Controller::hold(const std::string& reason, const double median) {
  reset_confirmation();
  stable_median_ = std::max(median, 1.0);
  filtered_log_median_ = std::log(*stable_median_);
  stats_.state = "stable";
  stats_.last_reason = reason;
  stats_.last_adjustment_factor = 1.0;
  update_state_stats();
}

void Controller::update_applied_controls(const AppliedControls& controls) noexcept {
  stats_.applied_controls_tracked = true;
  stats_.applied_controls_available =
      controls.left_exposure_us > 0 && controls.right_exposure_us > 0 &&
      std::isfinite(controls.left_analogue_gain) && controls.left_analogue_gain > 0.0 &&
      std::isfinite(controls.right_analogue_gain) && controls.right_analogue_gain > 0.0;
  // Sensor line timing and analogue-gain steps cannot reproduce every request
  // exactly. These tolerances cover quantization, not a previous 0.2 EV command.
  const auto exposure_tolerance =
      std::max(25.0, static_cast<double>(command_.exposure_us) * 0.01);
  // IMX296 uses floor(200*log10(gain)); one code step is about 1.16%.
  const auto gain_tolerance = command_.analogue_gain * 0.0125;
  stats_.applied_controls_match = stats_.applied_controls_available &&
      std::abs(static_cast<double>(controls.left_exposure_us) - command_.exposure_us) <= exposure_tolerance &&
      std::abs(static_cast<double>(controls.right_exposure_us) - command_.exposure_us) <= exposure_tolerance &&
      std::abs(controls.left_analogue_gain - command_.analogue_gain) <= gain_tolerance &&
      std::abs(controls.right_analogue_gain - command_.analogue_gain) <= gain_tolerance;
}

void Controller::update_state_stats() noexcept {
  stats_.settle_frames_remaining = settle_frames_remaining_;
  stats_.confirmation_observations = confirmation_observations_;
  stats_.confirmation_direction = confirmation_direction_;
  stats_.frame_period_us = timing_.frame_period_us;
  stats_.trigger_guard_us = timing_.trigger_guard_us;
  stats_.safe_max_exposure_us = safe_max_exposure_us();
  if (configuration_.mode == Mode::kAuto) {
    stats_.at_lower_limit =
        command_.exposure_us == effective_auto_min_exposure_us() &&
        command_.analogue_gain <= configuration_.auto_min_analogue_gain + 1e-9;
    stats_.at_upper_limit =
        command_.exposure_us == effective_auto_max_exposure_us() &&
        command_.analogue_gain >= configuration_.auto_max_analogue_gain - 1e-9;
  } else {
    stats_.at_lower_limit = command_.exposure_us == kAbsoluteMinimumExposureUs &&
                            command_.analogue_gain <= kAbsoluteMinimumGain + 1e-9;
    stats_.at_upper_limit = command_.exposure_us == safe_max_exposure_us() &&
                            command_.analogue_gain >= kAbsoluteMaximumGain - 1e-9;
  }
}

bool Controller::should_meter_next_frame() const noexcept {
  return configuration_.mode == Mode::kAuto &&
         (!stats_.applied_controls_tracked || stats_.applied_controls_match) &&
         settle_frames_remaining_ == 0U &&
         (stats_.frames_seen + 1U) %
                 configuration_.auto_update_interval_frames == 0U;
}

void Controller::skip_observation_frame() {
  if (should_meter_next_frame()) {
    throw std::logic_error("cannot skip a scheduled exposure observation");
  }
  // observe returns before inspecting either view on all non-metering frames.
  (void)observe({}, {});
}

std::optional<Command> Controller::observe(const ImageView& left,
                                           const ImageView& right) {
  ++stats_.frames_seen;
  if (configuration_.mode != Mode::kAuto) {
    stats_.state = "manual";
    update_state_stats();
    return std::nullopt;
  }
  if (stats_.applied_controls_tracked && !stats_.applied_controls_match) {
    ++stats_.applied_controls_wait_frames;
    stats_.state = stats_.applied_controls_available ? "awaiting_applied_controls"
                                                   : "applied_metadata_unavailable";
    stats_.last_reason = stats_.state;
    reset_confirmation();
    filtered_log_median_.reset();
    update_state_stats();
    return std::nullopt;
  }
  if (settle_frames_remaining_ > 0U) {
    --settle_frames_remaining_;
    stats_.state = "settling";
    update_state_stats();
    return std::nullopt;
  }
  if (stats_.frames_seen % configuration_.auto_update_interval_frames != 0U) {
    stats_.state = "waiting_interval";
    update_state_stats();
    return std::nullopt;
  }
  const auto observed = measure(left, right, metering_);
  ++stats_.observations;
  stats_.median = observed.median;
  stats_.left_median = observed.left_median;
  stats_.right_median = observed.right_median;
  stats_.median_difference =
      std::abs(observed.left_median - observed.right_median);
  stats_.dark_fraction = observed.dark_fraction;
  stats_.clipped_fraction = observed.clipped_fraction;
  stats_.left_clipped_fraction = observed.left_clipped_fraction;
  stats_.right_clipped_fraction = observed.right_clipped_fraction;

  const auto log_median = std::log(std::max(observed.median, 1.0));
  filtered_log_median_ = filtered_log_median_
      ? 0.5 * *filtered_log_median_ + 0.5 * log_median : log_median;
  const auto filtered_median = std::exp(*filtered_log_median_);

  bool scene_change_confirmed{};
  if (stable_median_) {
    // The stable baseline is fixed until a persistent scene change, so gradual
    // lighting changes accumulate instead of disappearing into a moving target.
    std::string direction;
    if (observed.median < *stable_median_ * 0.8 &&
        filtered_median < *stable_median_ * 0.85) direction = "dark";
    if (observed.median > *stable_median_ * 1.2 &&
        filtered_median > *stable_median_ * 1.15) direction = "bright";
    if (direction.empty()) {
      reset_confirmation();
      stats_.state = "stable";
      update_state_stats();
      return std::nullopt;
    }
    if (!confirm(direction)) return std::nullopt;
    stable_median_.reset();
    protect_highlights_ = false;
    scene_change_confirmed = true;
  }

  // Highlights select the lower end of the acceptable median band. They never
  // create a competing "keep darkening" objective below that band. A persistent
  // small saturated region can therefore coexist with usable shadow detail.
  const auto clipping = observed.clipped_fraction > metering_.max_clipped_fraction;
  // Retain this target through one convergence. Otherwise a quantized shutter
  // can repeatedly cross the clipping threshold and switch targets each step.
  protect_highlights_ = protect_highlights_ || clipping;
  const auto target = protect_highlights_ ? configuration_.auto_min_median
                                         : configuration_.auto_target_median;
  std::string direction;
  if (observed.median < configuration_.auto_min_median) direction = "dark";
  else if (observed.median > configuration_.auto_max_median ||
           (protect_highlights_ && observed.median > configuration_.auto_min_median * 1.05))
    direction = "bright";
  else {
    hold(protect_highlights_ ? "highlight_compromise" : "within_deadband", observed.median);
    return std::nullopt;
  }
  if (!scene_change_confirmed && !confirm(direction)) return std::nullopt;
  const auto step_ev = std::clamp(std::log2(target / filtered_median),
                                -kMaximumStepEv, kMaximumStepEv);
  // A transient must not leave the filter requesting the opposite direction.
  if ((direction == "dark" && step_ev <= 0.0) ||
      (direction == "bright" && step_ev >= 0.0)) {
    reset_confirmation();
    return std::nullopt;
  }
  const auto factor = std::exp2(step_ev);
  const auto reason = protect_highlights_ ? "highlight_compromise" : direction;

  const auto current_value = static_cast<double>(command_.exposure_us) *
                             command_.analogue_gain;
  stats_.last_adjustment_factor = factor;
  auto updated = split_exposure_value(current_value * factor, reason);
  const auto updated_value = static_cast<double>(updated.exposure_us) *
                             updated.analogue_gain;
  if (std::abs(updated_value - current_value) / current_value < 0.02) {
    hold("at_limit_or_quantized", observed.median);
    return std::nullopt;
  }
  command_ = updated;
  reset_confirmation();
  filtered_log_median_.reset();
  stats_.applied_controls_match = false;
  ++stats_.updates;
  settle_frames_remaining_ = configuration_.auto_settle_frames_after_update;
  stats_.state = "adjusted";
  update_state_stats();
  return command_;
}

const Configuration& Controller::configuration() const noexcept {
  return configuration_;
}

const Command& Controller::command() const noexcept { return command_; }

const Stats& Controller::stats() const noexcept { return stats_; }

}  // namespace depthd::exposure
