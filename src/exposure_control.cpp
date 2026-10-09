#include "depthd/exposure_control.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace depthd::exposure {
namespace {

constexpr std::uint32_t kAbsoluteMinimumExposureUs = 250U;
constexpr std::uint32_t kAbsoluteMaximumExposureUs = 8'000U;
constexpr double kAbsoluteMinimumGain = 1.0;
constexpr double kAbsoluteMaximumGain = 3.0;

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
                                  const ImageView& right) {
  if (left.data == nullptr || right.data == nullptr || left.width == 0U ||
      left.height == 0U || right.width == 0U || right.height == 0U ||
      left.stride_bytes < left.width || right.stride_bytes < right.width ||
      left.encoding != ImageEncoding::kGray8 ||
      right.encoding != ImageEncoding::kGray8 || left.width != right.width ||
      left.height != right.height) {
    throw std::invalid_argument(
        "auto exposure requires equal uint8 stereo luma images");
  }
  const auto x_begin = left.width / 5U;
  const auto x_end = left.width * 4U / 5U;
  const auto y_begin = left.height / 5U;
  const auto y_end = left.height * 4U / 5U;
  std::array<std::vector<std::uint8_t>, 2> side_pixels;
  const auto samples_per_image =
      static_cast<std::size_t>((x_end - x_begin + 3U) / 4U) *
      static_cast<std::size_t>((y_end - y_begin + 3U) / 4U);
  side_pixels[0].reserve(samples_per_image);
  side_pixels[1].reserve(samples_per_image);
  std::size_t dark{};
  std::array<std::size_t, 2> clipped{};
  const auto sample = [&](const ImageView& image, const std::size_t side) {
    for (auto y = y_begin; y < y_end; y += 4U) {
      const auto* row = image.data + static_cast<std::size_t>(y) *
                                         image.stride_bytes;
      for (auto x = x_begin; x < x_end; x += 4U) {
        const auto value = row[x];
        side_pixels[side].push_back(value);
        if (value < 16U) ++dark;
        if (value > 245U) ++clipped[side];
      }
    }
  };
  sample(left, 0U);
  sample(right, 1U);
  if (side_pixels[0].empty() || side_pixels[1].empty()) {
    throw std::invalid_argument("auto-exposure metering ROI is empty");
  }
  auto pixels = side_pixels[0];
  pixels.insert(pixels.end(), side_pixels[1].begin(), side_pixels[1].end());
  const auto median = [](std::vector<std::uint8_t>& values) {
    const auto middle = values.begin() +
                        static_cast<std::ptrdiff_t>(values.size() / 2U);
    std::nth_element(values.begin(), middle, values.end());
    return static_cast<double>(*middle);
  };
  const auto middle = pixels.begin() +
                      static_cast<std::ptrdiff_t>(pixels.size() / 2U);
  std::nth_element(pixels.begin(), middle, pixels.end());
  Observation result;
  result.median = static_cast<double>(*middle);
  result.left_median = median(side_pixels[0]);
  result.right_median = median(side_pixels[1]);
  result.dark_fraction = static_cast<double>(dark) /
                         static_cast<double>(pixels.size());
  result.left_clipped_fraction =
      static_cast<double>(clipped[0]) /
      static_cast<double>(side_pixels[0].size());
  result.right_clipped_fraction =
      static_cast<double>(clipped[1]) /
      static_cast<double>(side_pixels[1].size());
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
                       Configuration fallback, const TimingLimits timing)
    : control_file_(std::move(control_file)), fallback_(fallback),
      configuration_(fallback), timing_(timing) {
  validate(fallback_);
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
  const auto observed = measure(left, right);
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

  double factor{1.0};
  std::string reason;
  if (observed.clipped_fraction > 0.005) {
    reset_confirmation();
    factor = std::max(0.25, std::min(0.5, configuration_.auto_target_median /
                                             std::max(observed.median, 1.0)));
    reason = "clipping";
  } else if (observed.median < configuration_.auto_min_median ||
             observed.median > configuration_.auto_max_median) {
    const std::string direction =
        observed.median < configuration_.auto_min_median ? "dark" : "bright";
    if (confirmation_direction_ == direction) {
      ++confirmation_observations_;
    } else {
      confirmation_direction_ = direction;
      confirmation_observations_ = 1U;
    }
    if (confirmation_observations_ < 2U) {
      stats_.state = "confirming_" + direction;
      stats_.last_reason = stats_.state;
      update_state_stats();
      return std::nullopt;
    }
    factor = std::clamp(
        std::sqrt(configuration_.auto_target_median /
                  std::max(observed.median, 1.0)),
        0.5, 1.5);
    reason = direction;
  } else {
    reset_confirmation();
    stats_.state = "within_deadband";
    stats_.last_reason = "within_deadband";
    update_state_stats();
    return std::nullopt;
  }

  const auto current_value = static_cast<double>(command_.exposure_us) *
                             command_.analogue_gain;
  stats_.last_adjustment_factor = factor;
  auto updated = split_exposure_value(current_value * factor, reason);
  const auto updated_value = static_cast<double>(updated.exposure_us) *
                             updated.analogue_gain;
  if (std::abs(updated_value - current_value) / current_value < 0.02) {
    reset_confirmation();
    stats_.state = "at_limit_or_quantized";
    stats_.last_reason = "quantized_no_change";
    update_state_stats();
    return std::nullopt;
  }
  command_ = updated;
  reset_confirmation();
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
