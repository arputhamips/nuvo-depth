#include "depthd/exposure_control.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using depthd::exposure::Command;
using depthd::exposure::Configuration;
using depthd::exposure::Controller;

void require(const bool condition, const std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

double exposure_value(const Command& command) {
  return static_cast<double>(command.exposure_us) * command.analogue_gain;
}

void write_document(const std::filesystem::path& path, const std::string& document) {
  {
    std::ofstream output(path);
    output << document;
    if (!output) throw std::runtime_error("failed to write test configuration");
  }
  // Distinct updates even on filesystems with coarse timestamp resolution.
  static std::int64_t revision{};
  std::filesystem::last_write_time(path,
      std::filesystem::file_time_type::clock::now() + std::chrono::seconds(++revision));
}

void write_configuration(const std::filesystem::path& path, const Configuration& configuration) {
  write_document(path, depthd::exposure::configuration_json(configuration));
}

depthd::ImageView view(std::vector<std::uint8_t>& pixels) {
  return {pixels.data(), 64U, 48U, 64U, depthd::ImageEncoding::kGray8};
}

Configuration automatic_configuration() {
  Configuration result;
  result.mode = depthd::exposure::Mode::kAuto;
  result.manual_exposure_us = 8'000U;
  result.manual_analogue_gain = 1.0;
  return result;
}

// Fixed scene radiance, fed back through the real command on each frame.
// This catches control-loop cycles that prerecorded/uniform observations miss.
struct Scene {
  std::vector<std::uint8_t> left = std::vector<std::uint8_t>(64U * 48U);
  std::vector<std::uint8_t> right = std::vector<std::uint8_t>(64U * 48U);
  double radiance{75.0 / 8'000.0};
  double right_scale{1.0};
  bool left_highlight{};
  bool right_highlight{};

  void frame(Controller& controller, const double lighting = 1.0) {
    const auto old_value = exposure_value(controller.command());
    const auto pixel = [](const double value) {
      return static_cast<std::uint8_t>(std::clamp(std::round(value), 0.0, 255.0));
    };
    std::fill(left.begin(), left.end(), pixel(radiance * lighting * old_value));
    std::fill(right.begin(), right.end(), pixel(radiance * lighting * old_value * right_scale));
    // One of the 80 metering samples per camera: above the default 0.5% budget.
    if (left_highlight) left[9U * 64U + 12U] = pixel(400.0 * lighting * old_value / 8'000.0);
    if (right_highlight) right[9U * 64U + 12U] = pixel(400.0 * lighting * old_value / 8'000.0);
    if (controller.observe(view(left), view(right))) {
      const auto actual_step = std::abs(std::log2(exposure_value(controller.command()) / old_value));
      require(actual_step <= 0.24, "AE made an abrupt exposure jump");
    }
  }

  void run(Controller& controller, const std::uint32_t frames, const double lighting = 1.0) {
    for (std::uint32_t frame_index = 0; frame_index < frames; ++frame_index) frame(controller, lighting);
  }
};

void test_closed_loop(const std::filesystem::path& root) {
  for (const auto asymmetric : {false, true}) {
    Controller controller(root / "absent-hdr.json", automatic_configuration());
    Scene scene;
    scene.left_highlight = !asymmetric;
    scene.right_highlight = true;
    scene.right_scale = asymmetric ? 1.1 : 1.0;
    scene.run(controller, 160U);
    const auto stable_updates = controller.stats().updates;
    const auto stable_value = exposure_value(controller.command());
    scene.run(controller, 240U);
    require(controller.stats().updates == stable_updates && stable_updates < 8U,
            "stationary HDR scene continued to hunt");
    require(controller.stats().median && *controller.stats().median >= 60.0 &&
            *controller.stats().median <= 85.0,
            "HDR compromise sacrificed usable midtones");

    scene.run(controller, 240U, 0.5);
    require(exposure_value(controller.command()) > stable_value * 1.5,
            "persistent darker lighting did not increase exposure");
    const auto darker_updates = controller.stats().updates;
    scene.run(controller, 120U, 0.5);
    require(controller.stats().updates == darker_updates,
            "darker scene failed to settle");
    scene.run(controller, 240U, 2.0);
    require(exposure_value(controller.command()) < stable_value * 0.8,
            "persistent brighter lighting did not decrease exposure");
    const auto brighter_updates = controller.stats().updates;
    scene.run(controller, 120U, 2.0);
    require(controller.stats().updates == brighter_updates,
            "brighter scene failed to settle");
  }
}

void test_transients_and_gradual_change(const std::filesystem::path& root) {
  auto configuration = automatic_configuration();
  configuration.auto_update_interval_frames = 1U;
  configuration.auto_settle_frames_after_update = 0U;
  Controller controller(root / "absent-transients.json", configuration);
  Scene scene;
  scene.run(controller, 10U);
  for (std::uint32_t frame_index = 0; frame_index < 100U; ++frame_index) {
    scene.frame(controller, frame_index % 2U == 0U ? 0.9 : 1.1);
  }
  scene.run(controller, 2U, 3.0);
  scene.run(controller, 10U);
  scene.run(controller, 2U, 0.2);
  scene.run(controller, 10U);
  require(controller.stats().updates == 0U,
          "noise or a short lighting transient changed stable exposure");
  for (std::uint32_t frame_index = 0; frame_index < 30U; ++frame_index) {
    scene.frame(controller, frame_index % 2U == 0U ? 0.2 : 3.0);
  }
  require(controller.stats().updates == 0U,
          "alternating brightness bypassed persistent scene confirmation");
  scene.run(controller, 20U);
  for (std::uint32_t frame_index = 0; frame_index < 240U; ++frame_index) {
    scene.frame(controller, 1.0 - 0.5 * static_cast<double>(frame_index) / 239.0);
  }
  scene.run(controller, 120U, 0.5);
  require(exposure_value(controller.command()) > 11'200.0,
          "slow lighting drift disappeared into a moving baseline: " +
          std::to_string(exposure_value(controller.command())) + " state=" + controller.stats().state);
}

void test_quantized_hdr(const std::filesystem::path& root) {
  auto configuration = automatic_configuration();
  configuration.manual_exposure_us = 400U;
  configuration.auto_update_interval_frames = 1U;
  configuration.auto_settle_frames_after_update = 0U;
  Controller controller(root / "absent-quantized.json", configuration);
  std::vector<std::uint8_t> pixels(64U * 48U);
  const auto run = [&] {
    for (std::uint32_t frame_index = 0; frame_index < 120U; ++frame_index) {
      const auto value = static_cast<std::uint8_t>(std::clamp(
          std::round(63.0 * exposure_value(controller.command()) / 400.0), 0.0, 255.0));
      std::fill(pixels.begin(), pixels.end(), value);
      pixels[9U * 64U + 12U] = static_cast<std::uint8_t>(std::clamp(
          std::round(240.0 * exposure_value(controller.command()) / 400.0), 0.0, 255.0));
      (void)controller.observe(view(pixels), view(pixels));
    }
  };
  run();
  const auto updates = controller.stats().updates;
  run();
  require(controller.stats().updates == updates,
          "shutter quantization caused a fixed HDR scene to cycle");
}

void test_applied_controls(const std::filesystem::path& root) {
  auto configuration = automatic_configuration();
  configuration.manual_exposure_us = 2'000U;
  configuration.auto_update_interval_frames = 1U;
  configuration.auto_settle_frames_after_update = 3U;
  Controller controller(root / "absent-applied.json", configuration);
  std::vector<std::uint8_t> dark(64U * 48U, 20U);
  controller.update_applied_controls({2'000, 2'000, 1.0, 1.0});
  require(!controller.observe(view(dark), view(dark)), "first observation bypassed confirmation");
  require(!controller.observe(view(dark), view(dark)), "second observation bypassed confirmation");
  const auto updated = controller.observe(view(dark), view(dark));
  require(updated && updated->exposure_us > 2'000U && updated->analogue_gain == 1.0,
          "confirmed dark frame did not prefer shutter over gain");
  const auto updated_exposure = static_cast<std::int32_t>(updated->exposure_us);
  for (std::uint32_t frame_index = 0; frame_index < 20U; ++frame_index) {
    // Only the left camera has adopted the new shutter command.
    controller.update_applied_controls({updated_exposure, 2'000, 1.0, 1.0});
    require(!controller.should_meter_next_frame(), "stale stereo controls admitted metering");
    controller.skip_observation_frame();
    require(controller.stats().settle_frames_remaining == 3U,
            "settling counted frames before both cameras applied the command");
  }
  require(controller.stats().state == "awaiting_applied_controls" &&
          controller.stats().updates == 1U,
          "delayed sensor controls did not hold exposure");
  controller.update_applied_controls({});
  controller.skip_observation_frame();
  require(controller.stats().state == "applied_metadata_unavailable" &&
          controller.stats().applied_controls_wait_frames == 21U,
          "missing control metadata was silently treated as applied");
  for (std::uint32_t frame_index = 0; frame_index < 3U; ++frame_index) {
    controller.update_applied_controls({updated_exposure - 10, updated_exposure + 10, 1.005, 0.995});
    require(!controller.should_meter_next_frame(), "matching metadata skipped configured settling");
    controller.skip_observation_frame();
  }
  require(controller.should_meter_next_frame() && controller.stats().applied_controls_match,
          "quantized applied controls never released the settling gate");
  configuration.manual_exposure_us = 8'000U;
  configuration.manual_analogue_gain = 2.95;
  Controller gain_quantized(root / "absent-gain-quantized.json", configuration);
  const auto reported_gain = std::pow(10.0, std::floor(200.0 * std::log10(2.95)) / 200.0);
  gain_quantized.update_applied_controls({7'990, 7'990, reported_gain, reported_gain});
  require(gain_quantized.stats().applied_controls_match,
          "IMX296 logarithmic gain quantization prevented control acknowledgement");
}

void test_limits_and_metering(const std::filesystem::path& root) {
  auto configuration = automatic_configuration();
  configuration.auto_update_interval_frames = 1U;
  configuration.auto_settle_frames_after_update = 0U;
  Controller upper(root / "absent-upper.json", configuration, {6'000U, 250U});
  Scene dark;
  dark.radiance = 0.0001;
  dark.run(upper, 240U);
  require(upper.command().exposure_us == 5'750U && upper.command().analogue_gain == 3.0 &&
          upper.stats().at_upper_limit && upper.stats().safe_max_exposure_us == 5'750U,
          "dark scene did not hold at the guarded upper limit");
  const auto upper_updates = upper.stats().updates;
  dark.run(upper, 120U);
  require(upper.stats().updates == upper_updates, "unreachable dark target caused repeated updates");
  Controller lower(root / "absent-lower.json", configuration);
  std::vector<std::uint8_t> saturated(64U * 48U, 255U);
  for (std::uint32_t frame_index = 0; frame_index < 240U; ++frame_index)
    (void)lower.observe(view(saturated), view(saturated));
  require(lower.command().exposure_us == 250U && lower.command().analogue_gain == 1.0 &&
          lower.stats().at_lower_limit,
          "unreachable bright target did not hold at the lower limit");

  depthd::exposure::MeteringConfiguration metering;
  metering.roi = std::array<std::uint32_t, 4>{0U, 0U, 16U, 16U};
  metering.dark_threshold = 50U;
  metering.clipping_threshold = 100U;
  metering.max_clipped_fraction = 1.0;
  Controller custom(root / "absent-custom.json", configuration, {}, metering);
  std::vector<std::uint8_t> pixels(64U * 48U, 255U);
  for (std::uint32_t y = 0; y < 16U; ++y)
    for (std::uint32_t x = 0; x < 16U; ++x) pixels[y * 64U + x] = 42U;
  (void)custom.observe(view(pixels), view(pixels));
  require(custom.stats().median == 42.0 && custom.stats().dark_fraction == 1.0 &&
          custom.stats().clipped_fraction == 0.0,
          "profile ROI or dark threshold was ignored");
  metering.dark_threshold = 16U;
  metering.clipping_threshold = 40U;
  Controller clipping(root / "absent-clipping.json", configuration, {}, metering);
  (void)clipping.observe(view(pixels), view(pixels));
  require(clipping.stats().clipped_fraction == 1.0, "profile clipping threshold was ignored");
  std::fill(pixels.begin(), pixels.end(), 75U);
  Controller budget(root / "absent-budget.json", configuration, {}, metering);
  for (std::uint32_t frame_index = 0; frame_index < 12U; ++frame_index)
    (void)budget.observe(view(pixels), view(pixels));
  require(budget.stats().updates == 0U, "profile clipping fraction budget was ignored");
  metering.roi = std::array<std::uint32_t, 4>{0U, 0U, 65U, 48U};
  Controller outside(root / "absent-outside.json", configuration, {}, metering);
  try {
    (void)outside.observe(view(pixels), view(pixels));
    require(false, "out-of-image ROI was accepted");
  } catch (const std::invalid_argument&) {}
}

void test_configuration_and_gating(const std::filesystem::path& root) {
  const auto shipping = depthd::exposure::load_configuration(
      std::filesystem::path(DEPTHD_SOURCE_DIR) / "config/exposure.json");
  require(shipping.mode == depthd::exposure::Mode::kManual && shipping.manual_exposure_us == 8'000U,
          "shipping exposure defaults changed");
  require(depthd::exposure::parse_configuration(depthd::exposure::configuration_json(shipping))
              .manual_analogue_gain == 2.0,
          "existing live-control schema did not round-trip");
  auto invalid = shipping;
  invalid.manual_exposure_us = 8'001U;
  try { depthd::exposure::validate(invalid); require(false, "invalid shutter validated"); }
  catch (const std::invalid_argument&) {}
  try { (void)depthd::exposure::parse_configuration("{\"schema_version\":1}");
        require(false, "incomplete configuration parsed"); }
  catch (const std::invalid_argument&) {}

  const auto control = root / "exposure.json";
  auto manual = shipping;
  manual.manual_exposure_us = 4'000U;
  write_configuration(control, manual);
  Controller transitions(control, shipping);
  auto automatic = automatic_configuration();
  automatic.manual_exposure_us = 1'000U;
  write_configuration(control, automatic);
  const auto continuous = transitions.refresh();
  require(continuous && continuous->exposure_us == 4'000U && continuous->analogue_gain == 2.0,
          "manual-to-auto transition jumped to stale seed values");
  require(!transitions.refresh() && transitions.stats().configuration_reloads == 1U,
          "unchanged configuration reloaded");
  write_document(control, "{\"schema_version\":1}");
  require(!transitions.refresh() && !transitions.refresh() &&
          transitions.command().exposure_us == 4'000U && transitions.stats().configuration_errors == 1U,
          "invalid configuration did not preserve the last command and report once");
  manual.manual_exposure_us = 1'500U;
  manual.manual_analogue_gain = 1.5;
  write_configuration(control, manual);
  const auto manual_update = transitions.refresh();
  require(manual_update && manual_update->exposure_us == 1'500U && manual_update->analogue_gain == 1.5,
          "manual reload did not apply persisted controls");

  automatic.auto_update_interval_frames = 3U;
  automatic.auto_settle_frames_after_update = 4U;
  Controller reference(root / "absent-reference.json", automatic);
  Controller gated(root / "absent-gated.json", automatic);
  std::vector<std::uint8_t> dark(64U * 48U, 20U);
  std::uint32_t skipped{};
  for (std::uint32_t frame_index = 0; frame_index < 100U; ++frame_index) {
    const auto expected = reference.observe(view(dark), view(dark));
    std::optional<Command> actual;
    if (gated.should_meter_next_frame()) actual = gated.observe(view(dark), view(dark));
    else { gated.skip_observation_frame(); ++skipped; }
    require(actual.has_value() == expected.has_value() &&
            exposure_value(gated.command()) == exposure_value(reference.command()) &&
            gated.stats().frames_seen == reference.stats().frames_seen &&
            gated.stats().observations == reference.stats().observations &&
            gated.stats().settle_frames_remaining == reference.stats().settle_frames_remaining &&
            gated.stats().state == reference.stats().state,
            "DMA metering admission changed controller behavior");
  }
  require(skipped > 0U, "nonmetering frames were not gated");
}

}  // namespace

int main() {
  try {
    const auto root = std::filesystem::temp_directory_path() /
        ("nuvo-exposure-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    test_closed_loop(root);
    test_transients_and_gradual_change(root);
    test_quantized_hdr(root);
    test_applied_controls(root);
    test_limits_and_metering(root);
    test_configuration_and_gating(root);
    std::filesystem::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
