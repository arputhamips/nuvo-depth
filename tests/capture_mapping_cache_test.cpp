#include "depthd/runtime.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

#include <linux/dma-buf.h>
#include <linux/memfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
std::mutex fake_mutex;
std::set<std::uint64_t> fake_dma_inodes;
std::atomic<unsigned> starts{}, ends{}, releases{};
void require(const bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct Buffer {
  int fd{-1};
  explicit Buffer(const std::uint8_t value) {
    fd = static_cast<int>(::syscall(SYS_memfd_create, "nuvo-test-dma", MFD_CLOEXEC));
    require(fd >= 0 && ::ftruncate(fd, 18) == 0, "create fake capture buffer failed");
    // Padded I420: 4x2 Y with stride6, and 2x1 U/V with stride3.
    const std::array<std::uint8_t, 18> bytes{
        value, 32, 64, 235, 99, 99, 100, 110, 120, 130, 99, 99,
        128, 128, 99, 128, 128, 99};
    require(::pwrite(fd, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()),
            "populate fake capture buffer failed");
    struct stat info{}; require(::fstat(fd, &info) == 0, "stat fake buffer failed");
    std::lock_guard lock(fake_mutex); fake_dma_inodes.insert(static_cast<std::uint64_t>(info.st_ino));
  }
  ~Buffer() { if (fd >= 0) (void)::close(fd); }
};

depthd::StereoPair pair(const Buffer& left, const Buffer& right,
                        const std::uint64_t epoch, const std::uint64_t sequence) {
  depthd::StereoPair result;
  result.capture_epoch = epoch; result.pair_sequence = sequence; result.capture_timestamp_ns = 1000;
  for (const bool is_left : {true, false}) {
    auto& frame = is_left ? result.left : result.right;
    frame.role = is_left ? depthd::CameraRole::kLogicalLeft : depthd::CameraRole::kLogicalRight;
    frame.capture_epoch = epoch; frame.stream_sequence = sequence;
    frame.sensor_timestamp_ns = 1000; frame.sensor_clock = depthd::CaptureClock::kBoottime;
    frame.grid = depthd::make_reference_grid(4U, 2U); frame.pixel_format = "YUV420";
    depthd::CaptureBufferPlane plane;
    plane.fd = is_left ? left.fd : right.fd; plane.buffer_id = 1U;
    plane.length = 18U; plane.bytes_used = 18U; plane.stride_bytes = 6U;
    frame.planes.push_back(plane);
    frame.lease = depthd::CaptureBufferLease(epoch, sequence * 2U + (is_left ? 0U : 1U),
        [] { ++releases; });
  }
  return result;
}

void check_full_range_rgb(Buffer& left, const Buffer& right) {
  std::uint64_t sequence{10U};
  const auto check = [&](const std::array<std::uint8_t, 8>& luma,
                         const std::uint8_t cb, const std::uint8_t cr,
                         const bool separate_planes, const bool neutral) {
    // Keep non-image padding deliberately different from valid samples. Both
    // single-buffer and three-plane layouts must obey their offsets/strides.
    const std::array<std::uint8_t, 18> bytes{
        luma[0], luma[1], luma[2], luma[3], 99, 99,
        luma[4], luma[5], luma[6], luma[7], 99, 99,
        cb, cb, 99, cr, cr, 99};
    require(::pwrite(left.fd, bytes.data(), bytes.size(), 0) ==
                static_cast<ssize_t>(bytes.size()), "update color reference failed");
    auto captured = pair(left, right, 5U, sequence++);
    if (separate_planes) {
      auto& planes = captured.left.planes;
      auto y = planes.front();
      y.length = y.bytes_used = 12U;
      auto u = y;
      u.offset = 12U; u.length = u.bytes_used = 3U; u.stride_bytes = 3U;
      auto v = u;
      v.offset = 15U;
      planes = {y, u, v};
    }
    auto request = depthd::make_cpu_dmabuf_processing_request(std::move(captured));
    std::array<std::uint8_t, 24> rgb{};
    depthd::copy_capture_rgb(request, depthd::CameraRole::kLogicalLeft, rgb);
    for (std::size_t pixel = 0; pixel < luma.size(); ++pixel) {
      const auto y = static_cast<double>(luma[pixel]);
      const auto u = static_cast<double>(cb) - 128.0;
      const auto v = static_cast<double>(cr) - 128.0;
      // Independent Rec.601 equations, rather than repeating the production
      // fixed-point implementation. Include clipping and asymmetric chroma.
      const std::array<double, 3> expected{
          y + 1.402 * v, y - 0.3441362862 * u - 0.7141362862 * v,
          y + 1.772 * u};
      for (std::size_t channel = 0; channel < expected.size(); ++channel) {
        if (neutral) {
          require(rgb[pixel * 3U + channel] == luma[pixel],
                  "full-range neutral luma ramp was expanded or clipped");
        } else {
          const auto reference = std::lround(std::clamp(expected[channel], 0.0, 255.0));
          require(std::abs(static_cast<long>(rgb[pixel * 3U + channel]) - reference) <= 1,
                  "full-range RGB disagrees with Rec.601 chroma reference");
        }
      }
    }
  };
  for (unsigned base = 0U; base < 256U; base += 8U) {
    std::array<std::uint8_t, 8> ramp{};
    for (unsigned index = 0U; index < ramp.size(); ++index)
      ramp[index] = static_cast<std::uint8_t>(base + index);
    check(ramp, 128U, 128U, (base / 8U) % 2U != 0U, true);
  }
  const std::array<std::uint8_t, 8> samples{0, 16, 64, 128, 192, 235, 254, 255};
  for (const auto cb : {0U, 16U, 64U, 90U, 127U, 128U, 192U, 240U, 255U}) {
    for (const auto cr : {0U, 16U, 64U, 110U, 128U, 129U, 192U, 240U, 255U}) {
      for (const bool separate : {false, true})
        check(samples, static_cast<std::uint8_t>(cb), static_cast<std::uint8_t>(cr),
              separate, false);
    }
  }
}
}  // namespace

// Only this test's memfds emulate DMA_BUF_SYNC. Real ioctl calls are forwarded;
// the production mapper, mmap cache, descriptor handling, and copies are used.
extern "C" int ioctl(const int fd, const unsigned long operation, ...) noexcept {
  va_list arguments; va_start(arguments, operation);
  auto* argument = va_arg(arguments, void*); va_end(arguments);
  if (operation == DMA_BUF_IOCTL_SYNC) {
    struct stat info{};
    std::lock_guard lock(fake_mutex);
    if (::fstat(fd, &info) == 0 && fake_dma_inodes.contains(static_cast<std::uint64_t>(info.st_ino))) {
      const auto flags = static_cast<dma_buf_sync*>(argument)->flags;
      if ((flags & DMA_BUF_SYNC_END) != 0U) ++ends; else ++starts;
      return 0;
    }
  }
  return static_cast<int>(::syscall(SYS_ioctl, fd, operation, argument));
}

int main() {
  try {
    Buffer left(16U), right(45U);
    const auto before = depthd::cpu_dmabuf_mapping_stats();
    auto request = depthd::make_cpu_dmabuf_processing_request(pair(left, right, 3U, 1U),
        depthd::ColorPreviewRequest::kLeft);
    require(request.color_preview_yuv420.empty() &&
            depthd::cpu_dmabuf_mapping_stats().mappings_started == before.mappings_started,
            "request preparation copied or mapped capture before admission");
    auto first = request.dmabuf->acquire_cpu_fallback();
    auto again = request.dmabuf->acquire_cpu_fallback();
    require(first.left_owner == again.left_owner && first.right_owner == again.right_owner && starts == 2U,
            "one capture mapped/synchronized its luma repeatedly");
    request.left_mapping_owner = first.left_owner; request.right_mapping_owner = first.right_owner;
    request.left = first.left; request.right = first.right;
    first = {}; again = {};
    std::array<std::uint8_t, 8> mono{};
    std::array<std::uint8_t, 12> i420{};
    std::array<std::uint8_t, 24> rgb{};
    depthd::copy_capture_image(request, depthd::CameraRole::kLogicalLeft, false, mono);
    depthd::copy_capture_image(request, depthd::CameraRole::kLogicalLeft, true, i420);
    depthd::copy_capture_rgb(request, depthd::CameraRole::kLogicalLeft, rgb);
    require(mono == std::array<std::uint8_t, 8>{16,32,64,235,100,110,120,130},
            "mono copy included row padding");
    require(i420 == std::array<std::uint8_t, 12>{16,32,64,235,100,110,120,130,128,128,128,128},
            "I420 copy included padded luma/chroma");
    require(rgb[0] == 16U && rgb[1] == 16U && rgb[2] == 16U &&
            rgb[9] == 235U && rgb[10] == 235U && rgb[11] == 235U,
            "RGB conversion disagrees with neutral chroma endpoints");
    auto mapped = depthd::cpu_dmabuf_mapping_stats();
    require(mapped.mappings_started - before.mappings_started == 2U && mapped.mapping_cache_hits >= 2U,
            "admitted image copies re-created mappings for existing allocations");
    request = {};
    require(starts == ends && releases == 2U, "capture released with outstanding CPU synchronization");
    const auto synchronizations = starts.load();
    auto recycled = depthd::make_cpu_dmabuf_processing_request(pair(left, right, 3U, 2U));
    { auto fallback = recycled.dmabuf->acquire_cpu_fallback();
      require(fallback.left.data[0] == 16U, "cached mapping changed capture bytes"); }
    recycled = {};
    require(depthd::cpu_dmabuf_mapping_stats().mappings_started == mapped.mappings_started &&
            starts == synchronizations + 2U && starts == ends,
            "recycled allocation remapped or skipped fresh synchronization");
    auto restarted = depthd::make_cpu_dmabuf_processing_request(pair(left, right, 4U, 1U));
    { auto fallback = restarted.dmabuf->acquire_cpu_fallback(); (void)fallback; }
    restarted = {};
    auto after = depthd::cpu_dmabuf_mapping_stats();
    require(after.mappings_started == mapped.mappings_started + 2U && after.cached_mappings == 2U &&
            after.cached_bytes <= 64U * 1024U * 1024U && starts == ends,
            "new capture epoch reused a retired mapping or cache exceeded bounds");
    // A different allocation with the same buffer number and epoch must use
    // its inode/descriptor identity, including when the OS reuses an fd number.
    const auto original_fd = left.fd;
    (void)::close(left.fd); left.fd = -1;
    Buffer replacement(200U);
    require(replacement.fd == original_fd, "test did not reuse the capture descriptor number");
    auto replaced = depthd::make_cpu_dmabuf_processing_request(pair(replacement, right, 4U, 2U));
    { auto fallback = replaced.dmabuf->acquire_cpu_fallback();
      require(fallback.left.data[0] == 200U, "allocation replacement read stale mapped pixels"); }
    replaced = {};
    require(depthd::cpu_dmabuf_mapping_stats().mappings_started == after.mappings_started + 1U && starts == ends,
            "allocation replacement did not remap safely");
    check_full_range_rgb(replacement, right);
    require(starts == ends, "RGB references left DMA synchronization outstanding");
    std::cout << "capture_mapping_cache_test: pass\n";
  } catch (const std::exception& error) {
    std::cerr << "capture_mapping_cache_test: " << error.what() << '\n'; return 1;
  }
}
