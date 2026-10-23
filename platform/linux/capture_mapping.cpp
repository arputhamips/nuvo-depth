#include "depthd/runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <deque>
#include <mutex>

#if defined(__linux__)
#include <cerrno>
#include <atomic>
#include <cstring>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
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
std::atomic<std::uint64_t> mapping_cache_hits{};

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

struct MappingAllocation final {
  int fd{-1};
  void* address{MAP_FAILED};
  std::size_t bytes{}, data_offset{};
  std::uint64_t device{}, inode{}, epoch{}, buffer_id{}, offset{}, length{};
  ~MappingAllocation() noexcept {
    if (address != MAP_FAILED) (void)::munmap(address, bytes);
    if (fd >= 0) (void)::close(fd);
  }
};

struct MappingCache {
  std::mutex mutex;
  std::deque<std::shared_ptr<MappingAllocation>> entries;
  std::size_t bytes{};
};
MappingCache& mapping_cache() { static MappingCache cache; return cache; }

std::shared_ptr<MappingAllocation> acquire_mapping(const CaptureBufferPlane& plane,
                                                   const std::uint64_t epoch) {
  validate(plane);
  struct stat descriptor{};
  if (::fstat(plane.fd, &descriptor) != 0) mapping_error("identify DMA-BUF allocation");
  auto& cache = mapping_cache();
  std::lock_guard lock(cache.mutex);
  for (auto iterator = cache.entries.begin(); iterator != cache.entries.end();) {
    if ((*iterator)->epoch != epoch && iterator->use_count() == 1) {
      cache.bytes -= (*iterator)->bytes;
      iterator = cache.entries.erase(iterator);
    } else ++iterator;
  }
  auto found = std::find_if(cache.entries.begin(), cache.entries.end(), [&](const auto& entry) {
    return entry->device == static_cast<std::uint64_t>(descriptor.st_dev) &&
        entry->inode == static_cast<std::uint64_t>(descriptor.st_ino) &&
        entry->epoch == epoch && entry->buffer_id == plane.buffer_id &&
        entry->offset == plane.offset && entry->length == plane.length;
  });
  if (found != cache.entries.end()) {
    auto result = *found;
    cache.entries.erase(found); cache.entries.push_back(result);
    ++mapping_cache_hits;
    return result;
  }
  const auto page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) mapping_error("read DMA-BUF mapping page size");
  const auto page = static_cast<std::uint64_t>(page_size);
  const auto aligned_offset = plane.offset - plane.offset % page;
  if (aligned_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    throw std::overflow_error("DMA-BUF mapping offset exceeds off_t");
  const auto data_offset = static_cast<std::size_t>(plane.offset - aligned_offset);
  if (plane.length > std::numeric_limits<std::size_t>::max() - data_offset)
    throw std::overflow_error("DMA-BUF mapping length overflows size_t");
  const auto bytes = data_offset + static_cast<std::size_t>(plane.length);
  constexpr std::size_t capacity = 64U, budget = 64U * 1024U * 1024U;
  if (bytes > budget) throw std::length_error("capture mapping exceeds the 64 MiB cache budget");
  while (!cache.entries.empty() &&
         (cache.entries.size() >= capacity || cache.bytes > budget - bytes)) {
    const auto oldest = std::find_if(cache.entries.begin(), cache.entries.end(),
                                    [](const auto& entry) { return entry.use_count() == 1; });
    if (oldest == cache.entries.end())
      throw std::runtime_error("capture mapping cache is pinned at its bounded capacity");
    cache.bytes -= (*oldest)->bytes;
    cache.entries.erase(oldest);
  }
  auto result = std::make_shared<MappingAllocation>();
  result->fd = ::fcntl(plane.fd, F_DUPFD_CLOEXEC, 0);
  if (result->fd < 0) mapping_error("retain DMA-BUF allocation descriptor");
  result->bytes = bytes; result->data_offset = data_offset;
  result->device = static_cast<std::uint64_t>(descriptor.st_dev);
  result->inode = static_cast<std::uint64_t>(descriptor.st_ino);
  result->epoch = epoch; result->buffer_id = plane.buffer_id;
  result->offset = plane.offset; result->length = plane.length;
  result->address = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, result->fd,
                          static_cast<off_t>(aligned_offset));
  if (result->address == MAP_FAILED) mapping_error("map capture DMA-BUF plane");
  ++mapping_count;
  cache.entries.push_back(result); cache.bytes += bytes;
  return result;
}

class CpuDmaBufMapping final {
 public:
  CpuDmaBufMapping(const CaptureBufferPlane& plane, const std::uint64_t epoch)
      : allocation_(acquire_mapping(plane, epoch)) {
    if (synchronize_dmabuf(allocation_->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
                          sync_start_retries) != 0) {
      ++sync_start_failures;
      mapping_error("begin DMA-BUF CPU read access");
    }
    sync_active_ = true;
  }

  ~CpuDmaBufMapping() noexcept {
    end_sync();
  }

  CpuDmaBufMapping(const CpuDmaBufMapping&) = delete;
  CpuDmaBufMapping& operator=(const CpuDmaBufMapping&) = delete;

  [[nodiscard]] const std::uint8_t* data() const noexcept {
    return static_cast<const std::uint8_t*>(allocation_->address) + allocation_->data_offset;
  }

 private:
  void end_sync() noexcept {
    if (!sync_active_) {
      return;
    }
    if (synchronize_dmabuf(allocation_->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ,
                          sync_end_retries) != 0) {
      ++sync_end_failures;
    }
    sync_active_ = false;
  }

  std::shared_ptr<MappingAllocation> allocation_;
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

void copy_plane_rows(const CaptureBufferPlane& plane,
                     const std::uint32_t row_bytes,
                     const std::uint32_t rows,
                     std::uint8_t* destination, const std::uint64_t epoch) {
  validate(plane);
  if (destination == nullptr || row_bytes == 0U || rows == 0U ||
      plane.stride_bytes < row_bytes) {
    throw std::invalid_argument("captured color plane geometry is invalid");
  }
  const auto required = static_cast<std::uint64_t>(plane.stride_bytes) * rows;
  const auto populated = plane.bytes_used == 0U ? plane.length
                                                : plane.bytes_used;
  if (required > plane.length || required > populated) {
    throw std::invalid_argument(
        "captured color plane does not contain its declared rows");
  }
  const CpuDmaBufMapping mapping(plane, epoch);
  for (std::uint32_t row = 0; row < rows; ++row) {
    std::memcpy(destination + static_cast<std::size_t>(row) * row_bytes,
                mapping.data() + static_cast<std::size_t>(row) *
                                     plane.stride_bytes,
                row_bytes);
  }
}

void copy_yuv420(const CapturedFrame& frame,
                 const std::span<std::uint8_t> packed) {
  validate_frame_for_mapping(frame);
  if (frame.grid.width % 2U != 0U || frame.grid.height % 2U != 0U) {
    throw std::invalid_argument(
        "color preview requires even YUV420 capture dimensions");
  }
  const auto width = frame.grid.width;
  const auto height = frame.grid.height;
  const auto y_bytes = static_cast<std::size_t>(width) * height;
  const auto chroma_width = width / 2U;
  const auto chroma_height = height / 2U;
  const auto chroma_bytes =
      static_cast<std::size_t>(chroma_width) * chroma_height;
  if (packed.size() != y_bytes + 2U * chroma_bytes)
    throw std::invalid_argument("capture I420 destination size is invalid");

  if (frame.planes.size() >= 3U) {
    copy_plane_rows(frame.planes[0], width, height, packed.data(), frame.capture_epoch);
    copy_plane_rows(frame.planes[1], chroma_width, chroma_height,
                    packed.data() + y_bytes, frame.capture_epoch);
    copy_plane_rows(frame.planes[2], chroma_width, chroma_height,
                    packed.data() + y_bytes + chroma_bytes, frame.capture_epoch);
    return;
  }

  // Some allocators expose planar I420 through one contiguous DMA-BUF plane.
  // In that representation chroma rows follow the padded luma rows and use
  // half its stride.
  if (frame.planes.size() == 1U) {
    const auto& plane = frame.planes.front();
    const auto chroma_stride = (plane.stride_bytes + 1U) / 2U;
    const auto y_span = static_cast<std::uint64_t>(plane.stride_bytes) * height;
    const auto chroma_span =
        static_cast<std::uint64_t>(chroma_stride) * chroma_height;
    const auto required = y_span + 2U * chroma_span;
    const auto populated = plane.bytes_used == 0U ? plane.length
                                                  : plane.bytes_used;
    if (plane.stride_bytes < width || chroma_stride < chroma_width ||
        required > plane.length || required > populated) {
      throw std::invalid_argument(
          "contiguous captured YUV420 buffer has an invalid layout");
    }
    const CpuDmaBufMapping mapping(plane, frame.capture_epoch);
    for (std::uint32_t row = 0; row < height; ++row) {
      std::memcpy(packed.data() + static_cast<std::size_t>(row) * width,
                  mapping.data() +
                      static_cast<std::size_t>(row) * plane.stride_bytes,
                  width);
    }
    const auto* source_u = mapping.data() + y_span;
    const auto* source_v = source_u + chroma_span;
    for (std::uint32_t row = 0; row < chroma_height; ++row) {
      std::memcpy(packed.data() + y_bytes +
                      static_cast<std::size_t>(row) * chroma_width,
                  source_u + static_cast<std::size_t>(row) * chroma_stride,
                  chroma_width);
      std::memcpy(packed.data() + y_bytes + chroma_bytes +
                      static_cast<std::size_t>(row) * chroma_width,
                  source_v + static_cast<std::size_t>(row) * chroma_stride,
                  chroma_width);
    }
    return;
  }

  throw std::invalid_argument(
      "captured YUV420 color preview requires one or three planes");
}
#endif

}  // namespace

StereoProcessingRequest make_cpu_dmabuf_processing_request(
    StereoPair pair, const ColorPreviewRequest color_preview) {
#if defined(__linux__)
  validate_frame_for_mapping(pair.left);
  validate_frame_for_mapping(pair.right);
  StereoProcessingRequest result;
  result.capture = std::move(pair);
  result.color_preview = color_preview;
  result.left = {nullptr, result.capture.left.grid.width,
                 result.capture.left.grid.height,
                 result.capture.left.planes.front().stride_bytes,
                 ImageEncoding::kGray8, ImageMemory::kDmaMapping};
  result.right = {nullptr, result.capture.right.grid.width,
                  result.capture.right.grid.height,
                  result.capture.right.planes.front().stride_bytes,
                  ImageEncoding::kGray8, ImageMemory::kDmaMapping};
  result.left_dmabuf_lease = std::make_shared<CaptureRequestLeaseView>(
      result.capture.left.lease.token());
  result.right_dmabuf_lease = std::make_shared<CaptureRequestLeaseView>(
      result.capture.right.lease.token());
  const auto left_plane = result.capture.left.planes.front();
  const auto right_plane = result.capture.right.planes.front();
  const auto left_view = result.left;
  const auto right_view = result.right;
  const auto epoch = result.capture.capture_epoch;
  auto acquire_cpu_fallback =
      [left_plane, right_plane, left_view, right_view, epoch,
       left = std::shared_ptr<CpuDmaBufMapping>{},
       right = std::shared_ptr<CpuDmaBufMapping>{}]() mutable {
        if (!left) left = std::make_shared<CpuDmaBufMapping>(left_plane, epoch);
        if (!right) right = std::make_shared<CpuDmaBufMapping>(right_plane, epoch);
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
  (void)color_preview;
  throw std::runtime_error(
      "CPU DMA-BUF capture mapping is available only on Linux");
#endif
}

void copy_capture_image(const StereoProcessingRequest& request,
                        const CameraRole role, const bool color,
                        const std::span<std::uint8_t> destination) {
  if (role != CameraRole::kLogicalLeft && role != CameraRole::kLogicalRight)
    throw std::invalid_argument("capture image camera role is invalid");
  const bool left = role == CameraRole::kLogicalLeft;
  const auto& frame = left ? request.capture.left : request.capture.right;
  const auto& view = left ? request.left : request.right;
  const auto pixels = static_cast<std::size_t>(frame.grid.width) * frame.grid.height;
  if (destination.size() != (color ? pixels * 3U / 2U : pixels))
    throw std::invalid_argument("capture image destination size is invalid");
  if (color) {
    if ((request.color_preview == ColorPreviewRequest::kLeft && left) ||
        (request.color_preview == ColorPreviewRequest::kRight && !left)) {
      if (!request.color_preview_yuv420.empty()) {
        std::copy(request.color_preview_yuv420.begin(), request.color_preview_yuv420.end(),
                  destination.begin());
        return;
      }
    }
#if defined(__linux__)
    copy_yuv420(frame, destination);
    return;
#else
    throw std::runtime_error("capture I420 mapping is available only on Linux");
#endif
  }
  if (view.data != nullptr) {
    const auto channels = view.encoding == ImageEncoding::kGray8 ? 1U : 3U;
    for (std::size_t row = 0; row < view.height; ++row) {
      const auto* source = view.data + row * view.stride_bytes;
      auto* target = destination.data() + row * view.width;
      if (channels == 1U) std::copy_n(source, view.width, target);
      else {
        const auto red = view.encoding == ImageEncoding::kRgb8 ? 0U : 2U;
        for (std::size_t column = 0; column < view.width; ++column) {
          const auto index = column * 3U;
          target[column] = static_cast<std::uint8_t>((77U * source[index + red] +
              150U * source[index + 1U] + 29U * source[index + (2U - red)] + 128U) >> 8U);
        }
      }
    }
    return;
  }
#if defined(__linux__)
  validate_frame_for_mapping(frame);
  copy_plane_rows(frame.planes.front(), frame.grid.width, frame.grid.height, destination.data(),
                  frame.capture_epoch);
#else
  throw std::runtime_error("capture mono mapping is available only on Linux");
#endif
}

void copy_capture_rgb(const StereoProcessingRequest& request,
                      const CameraRole role, const std::span<std::uint8_t> destination) {
  if (role != CameraRole::kLogicalLeft && role != CameraRole::kLogicalRight)
    throw std::invalid_argument("capture RGB camera role is invalid");
  const bool left = role == CameraRole::kLogicalLeft;
  const auto& frame = left ? request.capture.left : request.capture.right;
  const auto& view = left ? request.left : request.right;
  const auto width = frame.grid.width, height = frame.grid.height;
  const auto pixels = static_cast<std::size_t>(width) * height;
  if (destination.size() != pixels * 3U)
    throw std::invalid_argument("capture RGB destination size is invalid");
#if defined(__linux__)
  if (frame.pixel_format == "YUV420" && !frame.planes.empty()) {
    validate_frame_for_mapping(frame);
    if (width % 2U != 0U || height % 2U != 0U)
      throw std::invalid_argument("capture RGB conversion requires even I420 dimensions");
    const auto map_plane = [&](const CaptureBufferPlane& plane, const std::uint32_t columns,
                              const std::uint32_t rows) {
      const auto populated = plane.bytes_used == 0U ? plane.length : plane.bytes_used;
      if (plane.stride_bytes < columns ||
          static_cast<std::uint64_t>(plane.stride_bytes) * rows > populated ||
          populated > plane.length)
        throw std::invalid_argument("capture RGB plane layout is invalid");
      return std::make_unique<CpuDmaBufMapping>(plane, frame.capture_epoch);
    };
    std::unique_ptr<CpuDmaBufMapping> y_mapping, u_mapping, v_mapping;
    const std::uint8_t* y{};
    const std::uint8_t* u{};
    const std::uint8_t* v{};
    std::size_t y_stride{}, chroma_stride{};
    const auto& y_plane = frame.planes.front();
    if (frame.planes.size() == 1U) {
      y_mapping = map_plane(y_plane, width, height);
      y = y_mapping->data();
      y_stride = y_plane.stride_bytes;
      chroma_stride = (y_stride + 1U) / 2U;
      const auto y_span = y_stride * height, chroma_span = chroma_stride * (height / 2U);
      const auto populated = y_plane.bytes_used == 0U ? y_plane.length : y_plane.bytes_used;
      if (y_span + chroma_span * 2U > populated)
        throw std::invalid_argument("capture RGB contiguous I420 layout is invalid");
      u = y + y_span;
      v = u + chroma_span;
    } else if (frame.planes.size() >= 3U) {
      if (view.data != nullptr && view.encoding == ImageEncoding::kGray8) {
        y = view.data;
        y_stride = view.stride_bytes;
      } else {
        y_mapping = map_plane(y_plane, width, height);
        y = y_mapping->data();
        y_stride = y_plane.stride_bytes;
      }
      u_mapping = map_plane(frame.planes[1], width / 2U, height / 2U);
      v_mapping = map_plane(frame.planes[2], width / 2U, height / 2U);
      if (frame.planes[1].stride_bytes != frame.planes[2].stride_bytes)
        throw std::invalid_argument("capture RGB chroma strides differ");
      u = u_mapping->data(); v = v_mapping->data();
      chroma_stride = frame.planes[1].stride_bytes;
    } else throw std::invalid_argument("capture RGB requires one or three I420 planes");
    const auto clip = [](const int value) {
      return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
    };
    for (std::size_t row = 0; row < height; ++row) {
      for (std::size_t column = 0; column < width; ++column) {
        const auto chroma_index = (row / 2U) * chroma_stride + column / 2U;
        // Native libcamera capture pins sYCC: full-range Rec.601 Y'CbCr.
        // Preserve the complete luma ramp instead of expanding video-range
        // 16..235. Sixteen-bit coefficients keep chroma rounding within one
        // code value of the Rec.601 floating-point equations.
        const int luminance = static_cast<int>(y[row * y_stride + column]) * 65536;
        const int blue = static_cast<int>(u[chroma_index]) - 128;
        const int red = static_cast<int>(v[chroma_index]) - 128;
        const auto index = (row * width + column) * 3U;
        destination[index] = clip((luminance + 91881 * red + 32768) >> 16);
        destination[index + 1U] = clip((luminance - 22553 * blue - 46802 * red + 32768) >> 16);
        destination[index + 2U] = clip((luminance + 116130 * blue + 32768) >> 16);
      }
    }
    return;
  }
#endif
  if (view.data == nullptr) throw std::invalid_argument("capture RGB has no mapped input");
  for (std::size_t row = 0; row < view.height; ++row) {
    const auto* source = view.data + row * view.stride_bytes;
    for (std::size_t column = 0; column < view.width; ++column) {
      const auto target = (row * view.width + column) * 3U;
      if (view.encoding == ImageEncoding::kGray8) {
        std::fill_n(destination.data() + target, 3U, source[column]);
      } else {
        const auto index = column * 3U;
        const auto red = view.encoding == ImageEncoding::kRgb8 ? 0U : 2U;
        destination[target] = source[index + red];
        destination[target + 1U] = source[index + 1U];
        destination[target + 2U] = source[index + (2U - red)];
      }
    }
  }
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
  auto& cache = mapping_cache();
  std::lock_guard lock(cache.mutex);
  return {mapping_count.load(), sync_start_retries.load(),
          sync_start_failures.load(), sync_end_retries.load(),
          sync_end_failures.load(), mapping_cache_hits.load(),
          cache.entries.size(), cache.bytes};
#else
  return {};
#endif
}

}  // namespace depthd
