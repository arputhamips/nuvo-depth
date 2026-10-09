#include "depthd/pio_pwm_trigger.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
void put(const std::filesystem::path& path, const std::string& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << value;
}
std::string get(const std::filesystem::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), {}};
}
}
int main() {
  const auto root = std::filesystem::temp_directory_path() /
      ("nuvo-pio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto chip = root / "pwmchip7";
  const auto channel = chip / "pwm0";
  put(chip / "device/of_node/compatible", std::string("raspberrypi,pwm-pio-rp1\0", 24));
  std::string gpios(12, '\0');
  gpios[3] = 1; gpios[7] = 17;
  put(chip / "device/of_node/gpios", gpios);
  put(channel / "enable", "1");  // An earlier daemon crashed while PWM ran.
  put(channel / "period", "0");
  put(channel / "duty_cycle", "0");
  put(channel / "polarity", "normal");
  if (depthd::PioPwmTrigger::discover(18, root)) return 1;
  auto pwm = depthd::PioPwmTrigger::discover(17, root);
  if (!pwm) return 2;
  if (get(channel / "enable") != "0") return 8;
  pwm->start(100'000'000ULL, 8'000, true);
  if (get(channel / "period") != "100000000" ||
      get(channel / "duty_cycle") != "8000000" ||
      get(channel / "polarity") != "inversed" ||
      get(channel / "enable") != "1") return 3;
  pwm->set_pulse(1500);
  if (get(channel / "duty_cycle") != "1500000") return 4;
  try { pwm->set_pulse(100000); return 5; }
  catch (const std::invalid_argument&) {}
  pwm->stop();
  if (get(channel / "enable") != "0") return 6;
  // A native/fan PWM controller must never be selected even if GPIO matches.
  put(chip / "device/of_node/compatible", "raspberrypi,rp1-pwm");
  if (depthd::PioPwmTrigger::discover(17, root)) return 7;
  pwm.reset();
  std::filesystem::remove_all(root);
}
