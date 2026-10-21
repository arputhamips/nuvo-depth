// SPDX-License-Identifier: GPL-3.0-or-later
// Terminal consumer of the existing local frame protocol. No camera ownership.
#include "depthd/local_frame_server.hpp"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
volatile std::sig_atomic_t stopped{};
extern "C" void stop(int) { stopped = 1; }

double number(const std::string& text) {
  std::size_t end{};
  const auto result = std::stod(text, &end);
  if (end != text.size() || !std::isfinite(result))
    throw std::invalid_argument("invalid numeric argument");
  return result;
}
}

int main(int argc, char** argv) {
  try {
    std::string socket = depthd::ipc_v2::kDataSocketPath;
    std::uint64_t frames = 1U;
    double cadence = 10.0;
    double timeout = 10.0;
    for (int index = 1; index < argc; ++index) {
      const std::string option = argv[index];
      if (option == "--help") {
        std::cout << "nuvo-depth-read [--socket PATH] [--frames N (0=continuous)] "
                     "[--hz N] [--timeout-seconds N]\nWrites associated raw depth and validity NuvoStream frames to stdout.\n";
        return 0;
      }
      if (++index >= argc) throw std::invalid_argument("missing value for " + option);
      const std::string value = argv[index];
      if (option == "--socket") socket = value;
      else if (option == "--hz") cadence = number(value);
      else if (option == "--timeout-seconds") timeout = number(value);
      else if (option == "--frames") {
        const auto count = number(value);
        if (count < 0.0 || count > 1'000'000.0 || std::floor(count) != count)
          throw std::invalid_argument("frames must be an integer in 0..1000000");
        frames = static_cast<std::uint64_t>(count);
      } else throw std::invalid_argument("unknown argument: " + option);
    }
    if (cadence < 0.001 || cadence > 60.0 || timeout < 1.0 || timeout > 120.0)
      throw std::invalid_argument("hz must be in 0.001..60 and timeout-seconds in 1..120");
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::signal(SIGPIPE, SIG_IGN);
    using namespace depthd::ipc_v2;
    LocalFrameClient client(socket, product_bit(ProductKind::kDepth) |
        product_bit(ProductKind::kValidity), cadence, 0.0);
    LocalFrameCursor after;
    bool primed{};
    std::uint64_t received{};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    while (!stopped && (frames == 0U || received < frames)) {
      const auto frame = client.next(after, std::chrono::milliseconds(400));
      if (!frame) {
        if (std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("timed out waiting for fresh depth; check nuvoctl status and service logs");
        continue;
      }
      (void)depthd::wire::deserialize(frame->bytes);
      // A previous client may have left a cached publication. Establish a
      // cursor first, then require a new complete identity for this reader.
      if (!primed) {
        after = frame->cursor;
        primed = true;
        continue;
      }
      std::cout.write(reinterpret_cast<const char*>(frame->bytes.data()),
                      static_cast<std::streamsize>(frame->bytes.size()));
      std::cout.flush();
      if (!std::cout) throw std::runtime_error("depth output closed or write failed");
      after = frame->cursor;
      ++received;
      deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    }
    return stopped ? 130 : 0;
  } catch (const std::exception& error) {
    std::cerr << "nuvo-depth-read: " << error.what() << '\n';
    return 2;
  }
}
