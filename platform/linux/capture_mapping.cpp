#include "depthd/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <atomic>
#include <cstring>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace depthd {
namespace {

#if defined(__linux__)
std::atomic<std::uint64_t> mapping_count{};
std::atomic<std::uint64_t> sync_start_retries{};
std::atomic<std::uint64_t> sync_start_failures{};
std::atomic<std::uint64_t> sync_end_retries{};
std::atomic<std::uint64_t> sync_end_failures{};

[[noreturn]] void mapping_error(const char* operation) {
  throw std::runtime_error(std::string(operation) + ": " +
                           std::strerror(errno));
}

int synchronize_dmabuf(const int fd, const std::uint64_t flags,
                       std::atomic<std::uint64_t>& retries) noexcept {
  struct dma_buf_sync operation { flags };
  while (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &operation) != 0) {
    if (errno != EINTR && errno != EAGAIN) {
      return -1;
    }
    ++retries;
  }
  return 0;
}

class CpuDmaBufMapping final {
 public:
  explicit CpuDmaBufMapping(const CaptureBufferPlane& plane) : fd_(plane.fd) {
    validate(plane);
    const auto page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
      mapping_error("read DMA-BUF mapping page size");
    }
    const auto page = static_cast<std::uint64_t>(page_size);
    const auto aligned_offset = plane.offset - plane.offset % page;
    if (aligned_offset >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
      throw std::overflow_error("DMA-BUF mapping offset exceeds off_t");
    }
    data_offset_ = static_cast<std::size_t>(plane.offset - aligned_offset);
    if (plane.length >
        std::numeric_limits<std::size_t>::max() - data_offset_) {
      throw std::overflow_error("DMA-BUF mapping length overflows size_t");
    }
    mapping_bytes_ = data_offset_ + static_cast<std::size_t>(plane.length);
    if (synchronize_dmabuf(fd_, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
                          sync_start_retries) != 0) {
      ++sync_start_failures;
      mapping_error("begin DMA-BUF CPU read access");
    }
    sync_active_ = true;
    mapping_ = ::mmap(nullptr, mapping_bytes_, PROT_READ, MAP_SHARED, fd_,
                      static_cast<off_t>(aligned_offset));
    if (mapping_ == MAP_FAILED) {
      mapping_ = nullptr;
      end_sync();
      mapping_error("map capture DMA-BUF luma plane");
    }
    ++mapping_count;
  }

  ~CpuDmaBufMapping() noexcept {
    if (mapping_ != nullptr) {
      (void)::munmap(mapping_, mapping_bytes_);
    }
    end_sync();
  }

  CpuDmaBufMapping(const CpuDmaBufMapping&) = delete;
  CpuDmaBufMapping& operator=(const CpuDmaBufMapping&) = delete;

  [[nodiscard]] const std::uint8_t* data() const noexcept {
    return static_cast<const std::uint8_t*>(mapping_) + data_offset_;
  }

 private:
  void end_sync() noexcept {
    if (!sync_active_) {
      return;
    }
    if (synchronize_dmabuf(fd_, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ,
                          sync_end_retries) != 0) {
      ++sync_end_failures;
    }
    sync_active_ = false;
  }

  int fd_{-1};
  void* mapping_{};
  std::size_t mapping_bytes_{};
  std::size_t data_offset_{};
  bool sync_active_{};
};

class CaptureRequestLeaseView final : public DmabufRequestLease {
 public:
  explicit CaptureRequestLeaseView(const std::uint64_t request_id)
      : request_id_(request_id) {
    if (request_id_ == 0U) {
      throw std::invalid_argument("DMA-BUF request identity must be nonzero");
    }
  }
  [[nodiscard]] bool valid() const noexcept override { return true; }
  [[nodiscard]] std::uint64_t request_id() const noexcept override {
    return request_id_;
  }

 private:
  std::uint64_t request_id_{};
};

void validate_frame_for_mapping(const CapturedFrame& frame) {
  if (frame.pixel_format != "YUV420") {
    throw std::invalid_argument(
        "native CPU capture mapping currently requires YUV420 luma");
  }
  if (frame.planes.empty()) {
    throw std::invalid_argument("captured frame has no luma plane");
  }
  const auto& plane = frame.planes.front();
  validate(plane);
  const auto required = static_cast<std::uint64_t>(plane.stride_bytes) *
                        frame.grid.height;
  if (plane.stride_bytes < frame.grid.width || required > plane.length) {
    throw std::invalid_argument(
        "captured luma plane does not cover the negotiated image grid");
  }
}

[[nodiscard]] DmabufPlaneView dmabuf_plane(
    const CapturedFrame& frame, const DmabufRequestLease* lease) {
  const auto& plane = frame.planes.front();
  if (plane.offset > std::numeric_limits<std::uint32_t>::max() ||
      plane.buffer_id == 0U || lease == nullptr || !lease->valid()) {
    throw std::invalid_argument("capture plane cannot be imported as DMA-BUF");
  }
  const auto allocation_identity =
      plane.buffer_id ^ (frame.capture_epoch * 0x9e3779b97f4a7c15ULL);
  return {plane.fd, allocation_identity, frame.grid.width, frame.grid.height,
          plane.stride_bytes, static_cast<std::uint32_t>(plane.offset),
          kDmabufModifierInvalid, DmabufPlaneFormat::kR8, lease};
}
#endif

}  // namespace

StereoProcessingRequest make_cpu_dmabuf_processing_request(StereoPair pair) {
#if defined(__linux__)
  validate_frame_for_mapping(pair.left);
  validate_frame_for_mapping(pair.right);
  StereoProcessingRequest result;
  result.capture = std::move(pair);
  result.left = {nullptr, result.capture.left.grid.width,
                 result.capture.left.grid.height,
                 result.capture.left.planes.front().stride_bytes,
                 ImageEncoding::kGray8};
  result.right = {nullptr, result.capture.right.grid.width,
                  result.capture.right.grid.height,
                  result.capture.right.planes.front().stride_bytes,
                  ImageEncoding::kGray8};
  result.left_dmabuf_lease = std::make_shared<CaptureRequestLeaseView>(
      result.capture.left.lease.token());
  result.right_dmabuf_lease = std::make_shared<CaptureRequestLeaseView>(
      result.capture.right.lease.token());
  const auto left_plane = result.capture.left.planes.front();
  const auto right_plane = result.capture.right.planes.front();
  const auto left_view = result.left;
  const auto right_view = result.right;
  auto acquire_cpu_fallback =
      [left_plane, right_plane, left_view, right_view]() mutable {
        auto left = std::make_shared<CpuDmaBufMapping>(left_plane);
        auto right = std::make_shared<CpuDmaBufMapping>(right_plane);
        auto mapped_left = left_view;
        auto mapped_right = right_view;
        mapped_left.data = left->data();
        mapped_right.data = right->data();
        return CpuFallbackStereoInput{left, right, mapped_left, mapped_right};
      };
  result.dmabuf = DmabufStereoInput{
      dmabuf_plane(result.capture.left, result.left_dmabuf_lease.get()),
      dmabuf_plane(result.capture.right, result.right_dmabuf_lease.get()),
      {}, {}, std::move(acquire_cpu_fallback)};
  validate(result);
  return result;
#else
  (void)pair;
  throw std::runtime_error(
      "CPU DMA-BUF capture mapping is available only on Linux");
#endif
}

bool cpu_dmabuf_mapping_available() noexcept {
#if defined(__linux__)
  return true;
#else
  return false;
#endif
}

CpuDmabufMappingStats cpu_dmabuf_mapping_stats() noexcept {
#if defined(__linux__)
  return {mapping_count.load(), sync_start_retries.load(),
          sync_start_failures.load(), sync_end_retries.load(),
          sync_end_failures.load()};
#else
  return {};
#endif
}

}  // namespace depthd
