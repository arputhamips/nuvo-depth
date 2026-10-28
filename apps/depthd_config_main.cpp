#include "depthd/config.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

constexpr int kUsageOrValidationError = 2;
constexpr int kIoError = 3;
constexpr int kInternalError = 70;

void print_usage() {
  std::cerr << "Usage:\n"
            << "  depthd-config --check-config PATH\n"
            << "  depthd-config --dump-effective-config PATH\n";
}

}  // namespace

// Keep service preflight independent of capture/backend dynamic libraries while
// using the same typed parser, validation and canonical JSON as depthd.
int main(const int argc, char** argv) {
  if (argc != 3) {
    print_usage();
    return kUsageOrValidationError;
  }
  const std::string_view command(argv[1]);
  if (command != "--check-config" && command != "--dump-effective-config") {
    std::cerr << "depthd-config: unknown command '" << command << "'\n";
    print_usage();
    return kUsageOrValidationError;
  }
  try {
    const std::filesystem::path path(argv[2]);
    const auto loaded = depthd::config::load(path);
    if (command == "--dump-effective-config") {
      std::cout << depthd::config::dump_json(loaded) << '\n';
      return 0;
    }
    std::cout << "configuration valid: " << path.string() << '\n'
              << "configuration sha256: " << loaded.configuration_sha256 << '\n';
    for (const auto& warning : loaded.warnings) {
      std::cout << "warning: " << warning.path << ": " << warning.message << '\n';
    }
    return 0;
  } catch (const depthd::config::ConfigError& error) {
    std::cerr << "depthd-config: "
              << (error.kind() == depthd::config::ConfigErrorKind::kIo
                      ? "configuration I/O error: "
                      : "configuration validation failed: ")
              << error.what() << '\n';
    return error.kind() == depthd::config::ConfigErrorKind::kIo
               ? kIoError
               : kUsageOrValidationError;
  } catch (const std::invalid_argument& error) {
    std::cerr << "depthd-config: usage or validation error: " << error.what() << '\n';
    return kUsageOrValidationError;
  } catch (const std::exception& error) {
    std::cerr << "depthd-config: configuration error: " << error.what() << '\n';
    return kInternalError;
  }
}
