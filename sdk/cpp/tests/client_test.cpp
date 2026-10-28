#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nuvo/client.hpp>
#include <stdexcept>
#include <thread>
#if defined(__linux__)
#include <sys/time.h>

#include <csignal>
#endif
namespace {
#if defined(__linux__)
std::size_t open_descriptors() {
  return static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator("/proc/self/fd"),
      std::filesystem::directory_iterator{}));
}
#endif
bool terminal_after_error(nuvo::client::Source& source) {
  bool rejected = false;
  try {
    (void)source.next();
  } catch (const std::exception&) {
    rejected = true;
  }
  if (!rejected || source.finished()) return false;
  // An invalid source must neither retry I/O nor disguise failure as clean EOF.
  return !source.next() && !source.next() && !source.finished();
}
}  // namespace
int main(int argc, char** argv) {
  using namespace nuvo::client;
  try {
    if (argc == 1) {
      auto sample = local_clock_sample();
      if (sample.boottime_ns <= 0 || sample.system_ns <= sample.boottime_ns ||
          sample.uncertainty_ns < 0)
        return 1;
      const std::array<std::byte, 5> unaligned{std::byte{7}, std::byte{0},
                                               std::byte{0}, std::byte{128},
                                               std::byte{63}};
      if (float32_le(unaligned.data() + 1) != 1.0F) return 1;
      try {
        Subscription s;
        s.products = {"depth", "depth"};
        validate(s);
        return 1;
      } catch (const std::invalid_argument&) {
      }
      return 0;
    }
    const std::string mode = argv[1];
    if (mode == "calibration") {
      const auto result = activate_local_calibration(argv[2], argv[3],
          argc > 4 ? std::chrono::milliseconds{std::stoi(argv[4])}
                   : std::chrono::milliseconds{3000});
      std::cout << result.calibration_sha256 << ' ' << result.calibration_authority
                << ' ' << result.run_uuid << ' ' << result.unchanged << '\n';
      return 0;
    }
    std::unique_ptr<Source> source;
#if defined(__linux__)
    const auto descriptors_before = open_descriptors();
#endif
    if (mode == "local" || mode == "cancel-local" || mode == "interrupt-local" ||
        mode == "terminal-local")
      source = local_source(argv[2]);
    else if (mode == "https" || mode == "cancel-https" ||
             mode == "terminal-https") {
      HttpsOptions options;
      options.url = argv[2];
      options.ca_file = argv[3];
      options.credential_file = argv[4];
      if (argc > 5) options.connect_address = argv[5];
      options.timeout = std::chrono::milliseconds(1500);
      source = https_source(options);
    } else if (mode == "replay" || mode == "terminal-replay" ||
               mode == "eof-replay")
      source = replay_source(argv[2]);
    else if (mode == "decode" || mode == "override" || mode == "validate") {
      std::ifstream input(argv[2], std::ios::binary);
      std::vector<char> data((std::istreambuf_iterator<char>(input)), {});
      auto frame = decode_frame(
          {reinterpret_cast<const std::byte*>(data.data()), data.size()});
      if (mode == "validate") {
        if (argc > 3 && std::string(argv[3]) == "constraints" &&
            (!frame.depth_constraints || !frame.disparity_constraints ||
             frame.depth_constraints->maximum_m != 20.0 ||
             frame.disparity_constraints->accepted_min_px != 0.5 ||
             frame.disparity_constraints->accepted_max_px != 64.0 ||
             frame.disparity_constraints->quantization_px != 0.0625 ||
             frame.disparity_constraints->valid_roi.width != 2))
          return 1;
        std::cout << frame.identity.capture_sequence << '\n';
        return 0;
      }
      if (mode == "override") {
        const auto original_authority = frame.calibration_authority;
        apply_calibration_override(frame, load_calibration_override(argv[3]));
        if (frame.calibration_authority != original_authority) return 1;
      }
      if (!frame.matching_geometry || !frame.find("depth") ||
          float32_le(frame.find("depth")->bytes.data()) !=
              (mode == "decode" && argc > 3 ? std::stof(argv[3]) : 2.0F))
        return 1;
      std::cout << frame.identity.capture_sequence << '\n';
      return 0;
    } else
      return 1;
    if (mode.starts_with("terminal-"))
      return terminal_after_error(*source) ? 0 : 1;
    if (mode == "eof-replay") {
      unsigned count{};
      while (!source->finished()) {
        if (source->next()) ++count;
      }
      if (source->next() || !source->finished()) return 1;
      std::cout << count << '\n';
      return 0;
    }
#if defined(__linux__)
    if (mode == "interrupt-local") {
      struct sigaction action {
      }, previous{};
      action.sa_handler = [](int) {};
      sigemptyset(&action.sa_mask);
      if (sigaction(SIGALRM, &action, &previous) != 0) return 1;
      itimerval signals{{0, 20000}, {0, 20000}}, disabled{};
      if (setitimer(ITIMER_REAL, &signals, nullptr) != 0) return 1;
      const auto started = std::chrono::steady_clock::now();
      bool rejected = false;
      try {
        (void)source->next();
      } catch (const std::exception&) {
        rejected = true;
      }
      setitimer(ITIMER_REAL, &disabled, nullptr);
      sigaction(SIGALRM, &previous, nullptr);
      const auto elapsed = std::chrono::steady_clock::now() - started;
      return rejected && elapsed >= std::chrono::milliseconds(1100) &&
                     elapsed < std::chrono::milliseconds(1800)
                 ? 0
                 : 1;
    }
#endif
    if (mode.starts_with("cancel-")) {
      std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        source->cancel();
      });
      const auto start = std::chrono::steady_clock::now();
      try {
        (void)source->next();
      } catch (const std::exception&) {
      }
      cancel.join();
      return !source->next() &&
                     std::chrono::steady_clock::now() - start <
                         std::chrono::seconds(2)
                 ? 0
                 : 1;
    }
    unsigned count{};
    std::vector<Frame> retained;
    std::vector<std::byte> first_wire;
    while (count < 2) {
      auto frame = source->next();
      if (!frame) {
        if (source->finished()) break;
        continue;
      }
      if (!frame->matching_geometry || !frame->find("depth") ||
          !frame->find("validity"))
        throw std::runtime_error("fixture products or geometry absent");
      if (retained.empty())
        first_wire.assign(frame->wire_bytes.begin(), frame->wire_bytes.end());
      retained.push_back(*frame);
      std::cout << frame->identity.capture_sequence << '\n';
      ++count;
    }
    source->cancel();
    source.reset();  // Both frames survive subsequent acquisition and teardown.
#if defined(__linux__)
    if (mode == "local" && open_descriptors() != descriptors_before)
      throw std::runtime_error("retained mapped frame kept an unnecessary fd");
#endif
    const auto expected =
        mode == "replay" && argc > 3 ? std::stof(argv[3]) : 2.0F;
    if (count == 0 || retained.empty() ||
        ((mode == "local" || mode == "https") && count != 2) ||
        !std::equal(first_wire.begin(), first_wire.end(),
                    retained.front().wire_bytes.begin(),
                    retained.front().wire_bytes.end()))
      return 1;
    for (const auto& frame : retained)
      if (float32_le(frame.find("depth")->bytes.data()) != expected) return 1;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
