#include "depthd/opencv_rectification_backend.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

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

  // Different starting pointers can still alias, including with padded rows.
  // Direct remap must reject both overlap directions before touching output.
  std::array<std::uint8_t, 64> overlapping;
  overlapping.fill(73U);
  for (const auto input_offset : {0U, 1U}) {
    for (const auto memory : {depthd::ImageMemory::kCpu, depthd::ImageMemory::kDmaMapping}) {
      bool rejected = false;
      try {
        backend->rectify(depthd::RectificationCamera::kLeft,
            {overlapping.data() + input_offset, width, height, width + 2U,
             depthd::ImageEncoding::kGray8, memory},
            {overlapping.data() + (1U - input_offset), width, height, width + 2U,
             depthd::ImageEncoding::kGray8}, stats);
      } catch (const std::invalid_argument&) { rejected = true; }
      if (!rejected || !std::all_of(overlapping.begin(), overlapping.end(),
                                  [](auto value) { return value == 73U; })) return 7;
    }
  }
  // Adjacent (nonoverlapping) pixel spans remain valid.
  backend->rectify(depthd::RectificationCamera::kLeft,
      {overlapping.data(), width, height, width, depthd::ImageEncoding::kGray8},
      {overlapping.data() + width * height, width, height, width, depthd::ImageEncoding::kGray8}, stats);

  // Exercise nonidentity maps, out-of-bounds interpolation, padded input/output,
  // both cameras, and repeated mono/RGB format switches in the reused cache.
  for (std::size_t i = 0; i < maps.left.source_x.size(); ++i) {
    maps.left.source_x[i] += (i % 3 == 0 ? -1.75F : 0.375F);
    maps.left.source_y[i] += 0.625F;
    maps.right.source_x[i] += 0.875F;
    maps.right.source_y[i] -= 1.125F;
  }
  config.border_value = 47;
  backend = registry.create("cpu_opencv", "", false, config, maps);
  for (int run = 0; run < 12; ++run) {
    const auto encoding = run % 3 == 0 ? depthd::ImageEncoding::kGray8 :
        (run % 3 == 1 ? depthd::ImageEncoding::kRgb8 : depthd::ImageEncoding::kBgr8);
    const auto channels = encoding == depthd::ImageEncoding::kGray8 ? 1U : 3U;
    const auto stride = width * channels + 5U;
    std::vector<std::uint8_t> input(stride * height, 231);
    std::vector<std::uint8_t> output(stride * height, 199);
    std::vector<std::uint8_t> expected(stride * height, 199);
    for (std::size_t i = 0; i < input.size(); ++i)
      input[i] = static_cast<std::uint8_t>(i * 37 + run * 13);
    for (const auto camera : {depthd::RectificationCamera::kLeft,
                              depthd::RectificationCamera::kRight}) {
      const auto& map = camera == depthd::RectificationCamera::kLeft ? maps.left : maps.right;
      cv::Mat x(height, width, CV_32FC1, const_cast<float*>(map.source_x.data()));
      cv::Mat y(height, width, CV_32FC1, const_cast<float*>(map.source_y.data()));
      cv::Mat fixed, interpolation;
      cv::convertMaps(x, y, fixed, interpolation, CV_16SC2);
      const auto type = channels == 1U ? CV_8UC1 : CV_8UC3;
      cv::Mat source(height, width, type, input.data(), stride);
      cv::Mat reference(height, width, type, expected.data(), stride);
      cv::remap(source, reference, fixed, interpolation, cv::INTER_LINEAR,
                cv::BORDER_CONSTANT, cv::Scalar::all(config.border_value));
      backend->rectify(camera, {input.data(), width, height, stride, encoding},
                        {output.data(), width, height, stride, encoding}, stats);
      if (output != expected) return 5;
    }
  }

  // Share immutable maps across workers, but never share their mutable DMA
  // staging buffers. Destroying the float source maps cannot invalidate them.
  auto prepared = depthd::prepare_opencv_rectification_maps(maps);
  auto second_backend = depthd::make_opencv_rectification_backend(config, prepared);
  backend = depthd::make_opencv_rectification_backend(config, prepared);
  maps = {};
  const auto run_worker = [&](depthd::RectificationBackend& worker, const std::uint8_t value) {
    std::array<std::uint8_t, width * height> input{}, direct{}, staged{};
    input.fill(value);
    for (int repeat = 0; repeat < 50; ++repeat) {
      depthd::RectificationStats direct_stats, dma_stats;
      worker.rectify(depthd::RectificationCamera::kLeft,
          {input.data(), width, height, width, depthd::ImageEncoding::kGray8},
          {direct.data(), width, height, width, depthd::ImageEncoding::kGray8}, direct_stats);
      worker.rectify(depthd::RectificationCamera::kLeft,
          {input.data(), width, height, width, depthd::ImageEncoding::kGray8, depthd::ImageMemory::kDmaMapping},
          {staged.data(), width, height, width, depthd::ImageEncoding::kGray8}, dma_stats);
      if (direct != staged || direct_stats.input_bytes_copied != 0U ||
          direct_stats.input_transfer_ms != 0.0 || dma_stats.input_bytes_copied != input.size()) return false;
    }
    return true;
  };
  auto other = std::async(std::launch::async, [&] { return run_worker(*second_backend, 203U); });
  if (!run_worker(*backend, 17U) || !other.get()) return 6;
  return 0;
}
