#include "depthd/exposure_control.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

void write_configuration(
    const std::filesystem::path& path,
    const depthd::exposure::Configuration& configuration) {
  std::ofstream output(path);
  output << depthd::exposure::configuration_json(configuration);
  if (!output) throw std::runtime_error("failed to write test configuration");
}

depthd::ImageView view(std::vector<std::uint8_t>& pixels) {
  return {pixels.data(), 64U, 48U, 64U, depthd::ImageEncoding::kGray8};
}

}  // namespace

int main() {
  try {
    const auto source = std::filesystem::path(DEPTHD_SOURCE_DIR) /
                        "config/exposure.json";
    const auto shipping = depthd::exposure::load_configuration(source);
    require(shipping.mode == depthd::exposure::Mode::kManual,
            "shipping exposure default must be manual");
    require(shipping.manual_exposure_us == 8'000U,
            "shipping manual exposure changed unexpectedly");
    require(depthd::exposure::parse_configuration(
                depthd::exposure::configuration_json(shipping))
                .manual_analogue_gain == 2.0,
            "exposure configuration did not round-trip");

    auto invalid = shipping;
    invalid.manual_exposure_us = 8'001U;
    try {
      depthd::exposure::validate(invalid);
      require(false, "out-of-range exposure unexpectedly validated");
    } catch (const std::invalid_argument&) {
    }
    try {
      (void)depthd::exposure::parse_configuration(
          "{\"schema_version\":1}");
      require(false, "incomplete configuration unexpectedly parsed");
    } catch (const std::invalid_argument&) {
    }

    auto automatic = shipping;
    automatic.mode = depthd::exposure::Mode::kAuto;
    automatic.manual_exposure_us = 2'000U;
    automatic.manual_analogue_gain = 1.0;
    automatic.auto_update_interval_frames = 1U;
    automatic.auto_settle_frames_after_update = 0U;
    const auto root = std::filesystem::temp_directory_path() /
                      "nuvo-exposure-control-test";
    std::filesystem::create_directories(root);
    const auto control = root / "exposure.json";
    write_configuration(control, automatic);

    std::vector<std::uint8_t> dark(64U * 48U, 20U);
    std::vector<std::uint8_t> target(64U * 48U, 75U);
    std::vector<std::uint8_t> bright(64U * 48U, 200U);
    std::vector<std::uint8_t> clipped(64U * 48U, 255U);
    auto dark_view = view(dark);
    auto target_view = view(target);
    auto bright_view = view(bright);
    auto clipped_view = view(clipped);

    depthd::exposure::Controller controller(control, shipping);
    require(!controller.observe(dark_view, dark_view),
            "one dark observation should be confirmed before adjustment");
    require(controller.stats().state == "confirming_dark",
            "dark confirmation state was not reported");
    const auto dark_command = controller.observe(dark_view, dark_view);
    require(dark_command.has_value(),
            "two dark observations did not request an auto update");
    require(dark_command->exposure_us > 2'000U,
            "auto exposure did not lengthen shutter for a dark frame");
    require(dark_command->analogue_gain == 1.0,
            "auto exposure raised gain before exhausting shutter time");
    require(controller.stats().updates == 1U,
            "auto exposure update counter is incorrect");

    const auto before_deadband = controller.command();
    require(!controller.observe(target_view, target_view),
            "deadband unexpectedly changed exposure");
    require(controller.command().exposure_us == before_deadband.exposure_us &&
                controller.command().analogue_gain ==
                    before_deadband.analogue_gain,
            "deadband did not preserve the shared command");

    const auto before_clipping =
        static_cast<double>(controller.command().exposure_us) *
        controller.command().analogue_gain;
    const auto clipping_command = controller.observe(target_view, clipped_view);
    require(clipping_command.has_value(),
            "clipping in one camera did not force an immediate reduction");
    require(static_cast<double>(clipping_command->exposure_us) *
                clipping_command->analogue_gain <
            before_clipping,
            "clipping response did not reduce exposure value");
    require(controller.stats().left_median == 75.0 &&
                controller.stats().right_median == 255.0 &&
                controller.stats().median_difference == 180.0,
            "per-camera luma/asymmetry telemetry is incorrect");
    require(controller.stats().right_clipped_fraction == 1.0,
            "per-camera clipping telemetry is incorrect");

    write_configuration(control, automatic);
    depthd::exposure::Controller alternating(control, shipping);
    for (std::uint32_t index = 0; index < 20U; ++index) {
      const auto selected = index % 2U == 0U ? dark_view : bright_view;
      require(!alternating.observe(selected, selected),
              "alternating brightness should not pass direction confirmation");
    }
    require(alternating.stats().updates == 0U,
            "alternating brightness caused controller oscillation");

    auto settling_configuration = automatic;
    settling_configuration.auto_settle_frames_after_update = 3U;
    write_configuration(control, settling_configuration);
    depthd::exposure::Controller settling(control, shipping);
    require(!settling.observe(dark_view, dark_view),
            "first dark observation bypassed confirmation");
    require(settling.observe(dark_view, dark_view).has_value(),
            "confirmed dark observation did not update");
    for (std::uint32_t frame = 0; frame < 3U; ++frame) {
      require(!settling.observe(bright_view, bright_view),
              "settling holdoff accepted stale observations");
    }
    require(settling.stats().settle_frames_remaining == 0U,
            "settling countdown did not reach zero");

    auto manual = shipping;
    manual.manual_exposure_us = 4'000U;
    manual.manual_analogue_gain = 2.0;
    write_configuration(control, manual);
    depthd::exposure::Controller transitions(control, shipping);
    auto enter_auto = automatic;
    enter_auto.manual_exposure_us = 1'000U;
    enter_auto.manual_analogue_gain = 1.0;
    write_configuration(control, enter_auto);
    const auto continuous = transitions.refresh();
    require(continuous && continuous->exposure_us == 4'000U &&
                continuous->analogue_gain == 2.0,
            "manual-to-auto transition jumped to stale seed values");
    require(transitions.stats().last_reason ==
                "configuration_auto_continuity",
            "manual-to-auto continuity reason was not reported");

    const auto command_before_invalid = transitions.command();
    {
      std::ofstream output(control);
      output << "{\"schema_version\":1}";
    }
    require(!transitions.refresh(),
            "invalid configuration unexpectedly produced a command");
    require(transitions.command().exposure_us ==
                command_before_invalid.exposure_us &&
                transitions.command().analogue_gain ==
                    command_before_invalid.analogue_gain &&
                transitions.stats().configuration_errors == 1U,
            "invalid configuration did not preserve the last command");

    write_configuration(control, automatic);
    depthd::exposure::Controller period_limited(
        control, shipping, {6'000U, 250U});
    require(period_limited.stats().safe_max_exposure_us == 5'750U,
            "dynamic period guard computed the wrong shutter ceiling");
    for (std::uint32_t frame = 0; frame < 40U; ++frame) {
      (void)period_limited.observe(dark_view, dark_view);
    }
    require(period_limited.command().exposure_us == 5'750U &&
                period_limited.command().analogue_gain == 3.0 &&
                period_limited.stats().at_upper_limit,
            "dark convergence exceeded or failed to reach guarded limits");

    auto manual_update = shipping;
    manual_update.manual_exposure_us = 1'500U;
    manual_update.manual_analogue_gain = 1.5;
    write_configuration(control, manual_update);
    const auto refreshed = period_limited.refresh();
    require(refreshed && refreshed->exposure_us == 1'500U &&
                refreshed->analogue_gain == 1.5,
            "manual configuration reload applied the wrong persisted value");

    // Skipped DMA mappings must preserve cadence, confirmation and settling.
    auto gated_configuration = automatic;
    gated_configuration.auto_update_interval_frames = 3U;
    gated_configuration.auto_settle_frames_after_update = 4U;
    write_configuration(control, gated_configuration);
    depthd::exposure::Controller reference(control, shipping);
    depthd::exposure::Controller gated(control, shipping);
    std::uint32_t skipped{};
    for (std::uint32_t frame = 0; frame < 40U; ++frame) {
      const auto expected = reference.observe(dark_view, dark_view);
      std::optional<depthd::exposure::Command> actual;
      if (gated.should_meter_next_frame()) actual = gated.observe(dark_view, dark_view);
      else { gated.skip_observation_frame(); ++skipped; }
      require(actual.has_value() == expected.has_value() &&
              gated.command().exposure_us == reference.command().exposure_us &&
              gated.command().analogue_gain == reference.command().analogue_gain &&
              gated.stats().frames_seen == reference.stats().frames_seen &&
              gated.stats().observations == reference.stats().observations &&
              gated.stats().settle_frames_remaining == reference.stats().settle_frames_remaining &&
              gated.stats().state == reference.stats().state,
              "metering admission changed exposure control behavior");
    }
    require(skipped > 0U, "no nonmetering frames were gated");
    std::filesystem::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
