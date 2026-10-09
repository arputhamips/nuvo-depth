#include "depthd/pio_pwm_trigger.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace depthd {
namespace {
std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
std::uint32_t be32(const std::string& bytes, std::size_t offset) {
  std::uint32_t value{};
  for (std::size_t i = 0; i < 4; ++i)
    value = (value << 8U) | static_cast<unsigned char>(bytes[offset + i]);
  return value;
}
}

std::unique_ptr<PioPwmTrigger> PioPwmTrigger::discover(
    std::uint32_t gpio, const std::filesystem::path& pwm_root) {
  if (!std::filesystem::exists(pwm_root)) return nullptr;
  for (const auto& chip : std::filesystem::directory_iterator(pwm_root)) {
    const auto node = chip.path() / "device/of_node";
    const auto compatible = read(node / "compatible");
    if (compatible.find("raspberrypi,pwm-pio-rp1") == std::string::npos)
      continue;
    const auto gpios = read(node / "gpios");
    if (gpios.size() != 12 || be32(gpios, 4) != gpio) continue;
    const auto channel = chip.path() / "pwm0";
    if (!std::filesystem::exists(channel / "enable"))
      throw std::runtime_error("RP1 PIO PWM channel is not exported; run nuvo-trigger-prepare");
    return std::unique_ptr<PioPwmTrigger>(new PioPwmTrigger(channel));
  }
  return nullptr;
}

PioPwmTrigger::PioPwmTrigger(std::filesystem::path channel)
    : channel_(std::move(channel)) {
  // A previous process may have been killed while the hardware kept running.
  // Discovery happens after camera acquisition, before either camera starts.
  write("enable", std::uint64_t{0});
}
PioPwmTrigger::~PioPwmTrigger() {
  try { stop(); } catch (...) {}
}
void PioPwmTrigger::write(const char* attribute, const char* value) {
  std::ofstream output(channel_ / attribute);
  output << value;
  output.close();
  if (!output) throw std::runtime_error(std::string("cannot set PIO PWM ") + attribute);
}
void PioPwmTrigger::write(const char* attribute, std::uint64_t value) {
  const auto text = std::to_string(value);
  write(attribute, text.c_str());
}
void PioPwmTrigger::start(std::uint64_t period_ns, std::uint32_t pulse_us,
                          bool idle_high) {
  if (!period_ns || static_cast<std::uint64_t>(pulse_us + 250ULL) * 1000ULL > period_ns)
    throw std::invalid_argument("PIO PWM exposure must leave a 250 us period guard");
  write("enable", std::uint64_t{0});
  // Linux PWM requires duty <= period while changing period. At startup the
  // channel may retain the last daemon's geometry.
  write("duty_cycle", std::uint64_t{0});
  write("period", period_ns);
  write("polarity", idle_high ? "inversed" : "normal");
  period_ns_ = period_ns;
  set_pulse(pulse_us);
  write("enable", std::uint64_t{1});
}
void PioPwmTrigger::set_pulse(std::uint32_t pulse_us) {
  if (!period_ns_ || static_cast<std::uint64_t>(pulse_us + 250ULL) * 1000ULL > period_ns_)
    throw std::invalid_argument("PIO PWM exposure must leave a 250 us period guard");
  write("duty_cycle", static_cast<std::uint64_t>(pulse_us) * 1000ULL);
}
void PioPwmTrigger::stop() {
  // RP1's driver holds the last pin level on disable; it does not promise an
  // idle-high level. Stop cameras before disabling to discard the final edge.
  write("enable", std::uint64_t{0});
}
}  // namespace depthd
