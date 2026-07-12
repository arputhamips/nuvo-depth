#include "depthd/opencv_rectification_backend.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace {

depthd::RectificationMap identity_map(std::uint32_t width,
                                     std::uint32_t height) {
  depthd::RectificationMap map;
  map.width = width;
  map.height = height;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      map.source_x.push_back(static_cast<float>(x));
      map.source_y.push_back(static_cast<float>(y));
    }
  }
  return map;
}

}  // namespace

int main() {
  constexpr std::uint32_t width = 4;
  constexpr std::uint32_t height = 3;
  depthd::StereoRectificationMaps maps;
  maps.left = identity_map(width, height);
  maps.right = identity_map(width, height);

  depthd::RectificationBackendConfig config;
  config.input_width = width;
  config.input_height = height;
  config.output_width = width;
  config.output_height = height;

  depthd::RectificationBackendRegistry registry;
  depthd::register_opencv_rectification_backend(registry);
  auto backend = registry.create("cpu_opencv", "", false, config, maps);

  std::array<std::uint8_t, width * height> left_input{};
  std::array<std::uint8_t, width * height> right_input{};
  std::array<std::uint8_t, width * height> left_output{};
  std::array<std::uint8_t, width * height> right_output{};
  for (std::size_t index = 0; index < left_input.size(); ++index) {
    left_input[index] = static_cast<std::uint8_t>(index + 1);
    right_input[index] = static_cast<std::uint8_t>(100 + index);
  }

  const depthd::ImageView left_view{left_input.data(), width, height, width,
                                    depthd::ImageEncoding::kGray8};
  const depthd::ImageView right_view{right_input.data(), width, height, width,
                                     depthd::ImageEncoding::kGray8};
  const depthd::MutableImageView left_destination{
      left_output.data(), width, height, width, depthd::ImageEncoding::kGray8};
  const depthd::MutableImageView right_destination{
      right_output.data(), width, height, width, depthd::ImageEncoding::kGray8};

  depthd::RectificationStats stats;
  backend->rectify_pair(left_view, right_view, left_destination,
                        right_destination, stats);
  if (!std::equal(left_input.begin(), left_input.end(), left_output.begin()) ||
      !std::equal(right_input.begin(), right_input.end(),
                  right_output.begin())) {
    return 1;
  }
  if (stats.total_ms < 0.0 || stats.compute_ms < 0.0 ||
      stats.input_transfer_ms != 0.0 || stats.output_transfer_ms != 0.0) {
    return 2;
  }

  constexpr std::size_t bgr_stride = width * 3 + 5;
  std::array<std::uint8_t, bgr_stride * height> bgr_input{};
  std::array<std::uint8_t, bgr_stride * height> bgr_output{};
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width * 3; ++x) {
      bgr_input[static_cast<std::size_t>(y) * bgr_stride + x] =
          static_cast<std::uint8_t>(10 * y + x);
    }
  }
  const depthd::ImageView bgr_view{bgr_input.data(), width, height, bgr_stride,
                                   depthd::ImageEncoding::kBgr8};
  const depthd::MutableImageView bgr_destination{
      bgr_output.data(), width, height, bgr_stride,
      depthd::ImageEncoding::kBgr8};
  backend->rectify(depthd::RectificationCamera::kLeft, bgr_view,
                   bgr_destination, stats);
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto row_offset = static_cast<std::size_t>(y) * bgr_stride;
    if (!std::equal(bgr_input.begin() + row_offset,
                    bgr_input.begin() + row_offset + width * 3,
                    bgr_output.begin() + row_offset)) {
      return 3;
    }
  }

  try {
    auto invalid_output = left_destination;
    invalid_output.encoding = depthd::ImageEncoding::kBgr8;
    backend->rectify(depthd::RectificationCamera::kLeft, left_view,
                     invalid_output, stats);
    return 4;
  } catch (const std::invalid_argument&) {
  }

  return 0;
}
