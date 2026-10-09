#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace depthd {

// Only discovers RP1 PIO PWM channels bound to the requested GPIO, never the
// RP1 fan/native PWM controller. Provisioning and export require the root helper.
class PioPwmTrigger final {
 public:
  static std::unique_ptr<PioPwmTrigger> discover(
      std::uint32_t gpio,
      const std::filesystem::path& pwm_root = "/sys/class/pwm");
  ~PioPwmTrigger();
  void start(std::uint64_t period_ns, std::uint32_t pulse_us, bool idle_high);
  void set_pulse(std::uint32_t pulse_us);
  void stop();
 private:
  explicit PioPwmTrigger(std::filesystem::path channel);
  void write(const char* attribute, const char* value);
  void write(const char* attribute, std::uint64_t value);
  std::filesystem::path channel_;
  std::uint64_t period_ns_{};
};

}  // namespace depthd
