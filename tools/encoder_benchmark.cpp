#include "depthd/image_encoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

std::uint32_t number(const char* value, const char* option) {
  const auto parsed = std::strtoul(value, nullptr, 10);
  if (parsed == 0UL || parsed > 50'000'000UL) {
    throw std::invalid_argument(std::string(option) + " is out of range");
  }
  return static_cast<std::uint32_t>(parsed);
}

double percentile(std::vector<double> values, const double fraction) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(
      fraction * static_cast<double>(values.size() - 1U));
  return values[index];
}

depthd::FrameSetStorage frame(const std::uint32_t width,
                              const std::uint32_t height) {
  depthd::FrameSetStorage result;
  result.metadata.frame.capture_epoch = 1U;
  result.metadata.frame.capture_sequence = 1U;
  result.metadata.frame.configuration_epoch = 1U;
  result.metadata.frame.capture_time_ns = 1;
  result.metadata.frame.capture_clock = depthd::CaptureClock::kBoottime;
  const auto grid = depthd::make_reference_grid(width, height);
  const depthd::RegionU32 roi{0U, 0U, width, height};
  result.metadata.left_rectified = {
      grid, roi, depthd::CameraRole::kLogicalLeft,
      depthd::ImageProductFormat::kGray8};
  const auto pixels = static_cast<std::size_t>(width) * height;
  result.left_rectified.resize(pixels);
  for (std::uint32_t row = 0; row < height; ++row) {
    for (std::uint32_t column = 0; column < width; ++column) {
      result.left_rectified[static_cast<std::size_t>(row) * width + column] =
          static_cast<std::uint8_t>((column * 3U + row * 5U +
                                     ((column / 32U) ^ (row / 32U)) * 31U) &
                                    0xffU);
    }
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    depthd::encoding::Configuration config;
    config.mode = depthd::encoding::StreamMode::kEncodedOnly;
    std::uint32_t width = 1920U;
    std::uint32_t height = 1080U;
    std::uint32_t frames = 180U;
    for (int index = 1; index < argc; index += 2) {
      if (index + 1 >= argc) throw std::invalid_argument("option needs value");
      const std::string_view option(argv[index]);
      const std::string_view value(argv[index + 1]);
      if (option == "--codec") {
        if (value == "h264") config.codec = depthd::encoding::Codec::kH264;
        else if (value == "jpeg") config.codec = depthd::encoding::Codec::kJpeg;
        else throw std::invalid_argument("unsupported codec");
      } else if (option == "--width") width = number(argv[index + 1], argv[index]);
      else if (option == "--height") height = number(argv[index + 1], argv[index]);
      else if (option == "--frames") frames = number(argv[index + 1], argv[index]);
      else if (option == "--fps") config.cadence_hz = number(argv[index + 1], argv[index]);
      else if (option == "--bitrate") config.bitrate_bps = number(argv[index + 1], argv[index]);
      else if (option == "--quality") config.jpeg_quality = number(argv[index + 1], argv[index]);
      else if (option == "--threads") config.threads = number(argv[index + 1], argv[index]);
      else if (option == "--preset") config.preset = value;
      else throw std::invalid_argument("unknown option");
    }
    config.width = width;
    config.height = height;
    depthd::encoding::validate(config);
    auto input = frame(width, height);
    auto encoder = depthd::encoding::make_ffmpeg_encoder(config);
    std::vector<double> samples;
    samples.reserve(frames);
    std::uint64_t bytes{};
    const auto total_started = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < frames; ++index) {
      input.metadata.frame.capture_sequence = index + 1U;
      input.metadata.frame.capture_time_ns =
          static_cast<std::int64_t>(index + 1U) * 1'000'000'000LL /
          config.cadence_hz;
      const auto encoded = encoder->encode(input, index + 1U);
      samples.push_back(encoded.encode_ms);
      bytes += encoded.bytes.size();
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - total_started).count();
    std::cout << std::fixed << std::setprecision(3)
              << "{\"codec\":\"" << depthd::encoding::name(config.codec)
              << "\",\"width\":" << width << ",\"height\":" << height
              << ",\"frames\":" << frames << ",\"configured_fps\":"
              << config.cadence_hz << ",\"achieved_fps\":"
              << static_cast<double>(frames) / elapsed << ",\"encode_ms_p50\":"
              << percentile(samples, 0.50) << ",\"encode_ms_p95\":"
              << percentile(samples, 0.95) << ",\"encode_ms_p99\":"
              << percentile(samples, 0.99) << ",\"average_bytes\":"
              << bytes / frames << ",\"bitrate_bps\":" << config.bitrate_bps
              << ",\"threads\":" << config.threads << ",\"preset\":\""
              << config.preset << "\"}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "encoder_benchmark: " << error.what() << '\n';
    return 2;
  }
}
