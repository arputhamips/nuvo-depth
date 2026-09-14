#include "depthd/result_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace depthd {
namespace {

constexpr std::size_t kExpectedDisparityElements =
    static_cast<std::size_t>(ipc::kImageWidth) * ipc::kImageHeight;

template <std::size_t Size>
void copy_string(std::array<char, Size>& destination,
                 const std::string& value) {
  if (value.empty() || value.size() >= Size) {
    throw std::invalid_argument("result-ring frame id is empty or too long");
  }
  destination.fill('\0');
  std::memcpy(destination.data(), value.data(), value.size());
}

void validate_frame_id(const std::string& value) {
  if (value.empty() || value.size() >= ipc::kFrameIdCapacity) {
    throw std::invalid_argument("result-ring frame id is empty or too long");
  }
}

std::uint64_t load_acquire(const std::uint64_t* value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#else
  const auto loaded = *reinterpret_cast<volatile const std::uint64_t*>(value);
  std::atomic_thread_fence(std::memory_order_acquire);
  return loaded;
#endif
}

void store_release(std::uint64_t* destination,
                   const std::uint64_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __atomic_store_n(destination, value, __ATOMIC_RELEASE);
#else
  std::atomic_thread_fence(std::memory_order_release);
  *reinterpret_cast<volatile std::uint64_t*>(destination) = value;
#endif
}

void store_relaxed(std::uint64_t* destination,
                   const std::uint64_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __atomic_store_n(destination, value, __ATOMIC_RELAXED);
#else
  *reinterpret_cast<volatile std::uint64_t*>(destination) = value;
#endif
}

std::int64_t realtime_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::uint64_t make_instance_id() {
  std::random_device device;
  const auto time = static_cast<std::uint64_t>(realtime_now_ns());
  const auto random = (static_cast<std::uint64_t>(device()) << 32U) |
                      static_cast<std::uint64_t>(device());
  const auto mixed = time ^ random;
  return mixed == 0U ? 1U : mixed;
}

bool valid_ring_header(const ipc::ResultRingHeader& header) noexcept {
  return header.magic == ipc::kResultRingMagic &&
         header.schema_version == ipc::kResultRingSchemaVersion &&
         header.endian_tag == ipc::kLittleEndianTag &&
         header.header_bytes == ipc::kResultRingHeaderBytes &&
         header.slot_header_bytes == ipc::kResultSlotHeaderBytes &&
         header.slot_count == ipc::kResultRingSlotCount &&
         header.slot_stride_bytes == ipc::kResultSlotStrideBytes &&
         header.file_bytes == ipc::kResultRingFileBytes &&
         header.disparity_offset == ipc::kDisparityOffset &&
         header.disparity_capacity_bytes == ipc::kDisparityCapacityBytes &&
         header.mono_offset == ipc::kMonoOffset &&
         header.mono_capacity_bytes == ipc::kMonoCapacityBytes &&
         header.jpeg_offset == ipc::kJpegOffset &&
         header.jpeg_capacity_bytes == ipc::kJpegCapacityBytes &&
         header.obstacle_offset == ipc::kObstacleOffset &&
         header.obstacle_capacity_bytes == ipc::kObstacleCapacityBytes &&
         header.instance_id != 0U;
}

bool valid_slot_header(const ipc::ResultSlotHeader& slot) noexcept {
  const auto obstacle_bytes =
      static_cast<std::uint64_t>(slot.obstacle_point_count) *
      3U * sizeof(float);
  const auto required_flags = static_cast<std::uint32_t>(
      ipc::ProductFlags::kDisparity | ipc::ProductFlags::kLeftCameraInfo |
      ipc::ProductFlags::kRightCameraInfo |
      ipc::ProductFlags::kObstacleCloud);
  const bool mono_consistent =
      ipc::has_flag(slot.product_flags,
                    ipc::ProductFlags::kLeftRectifiedMono) ==
      (slot.mono_bytes > 0U);
  const bool jpeg_consistent =
      ipc::has_flag(slot.product_flags,
                    ipc::ProductFlags::kLeftRectifiedJpeg) ==
      (slot.jpeg_bytes > 0U);
  const auto terminated = [](const auto& value) {
    return std::memchr(value.data(), '\0', value.size()) != nullptr;
  };
  return (slot.product_flags & required_flags) == required_flags &&
         mono_consistent && jpeg_consistent &&
         slot.instance_id != 0U && slot.sequence != 0U &&
         slot.width == ipc::kImageWidth &&
         slot.height == ipc::kImageHeight &&
         slot.disparity_stride_bytes == ipc::kDisparityStrideBytes &&
         slot.disparity_bytes == ipc::kDisparityCapacityBytes &&
         slot.mono_bytes <= ipc::kMonoCapacityBytes &&
         slot.jpeg_bytes <= ipc::kJpegCapacityBytes &&
         slot.obstacle_point_count <= ipc::kObstaclePointCapacity &&
         obstacle_bytes <= ipc::kObstacleCapacityBytes &&
         slot.left_camera.distortion_count <=
             ipc::kDistortionCoefficientCapacity &&
         slot.right_camera.distortion_count <=
             ipc::kDistortionCoefficientCapacity &&
         terminated(slot.left_optical_frame) &&
         terminated(slot.right_optical_frame) &&
         terminated(slot.obstacle_frame) &&
         slot.valid_window_x_offset + slot.valid_window_width <= slot.width &&
         slot.valid_window_y_offset + slot.valid_window_height <= slot.height;
}

void validate_snapshot(const ResultSnapshotView& snapshot) {
  if (snapshot.capture_realtime_ns <= 0 || snapshot.ready_realtime_ns <= 0 ||
      snapshot.ready_realtime_ns < snapshot.capture_realtime_ns) {
    throw std::invalid_argument("result-ring timestamps are invalid");
  }
  if (snapshot.disparity.size() != kExpectedDisparityElements) {
    throw std::invalid_argument(
        "result-ring disparity must be exactly 728x544 float32");
  }
  if (!snapshot.left_rectified_mono.empty() &&
      snapshot.left_rectified_mono.size() != ipc::kMonoCapacityBytes) {
    throw std::invalid_argument(
        "result-ring mono image must be empty or exactly 728x544 bytes");
  }
  if (snapshot.left_rectified_jpeg.size() > ipc::kJpegCapacityBytes) {
    throw std::invalid_argument("result-ring JPEG exceeds fixed capacity");
  }
  if (snapshot.obstacle_points.size() > ipc::kObstaclePointCapacity) {
    throw std::invalid_argument(
        "result-ring obstacle cloud exceeds fixed capacity");
  }
}

#if defined(__linux__) || defined(__APPLE__)
class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile() { close(); }
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  void close() noexcept {
    if (mapping_ != nullptr) {
      ::munmap(mapping_, mapping_size_);
      mapping_ = nullptr;
      mapping_size_ = 0;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    device_ = 0;
    inode_ = 0;
  }

  void open_writer(const std::string& path, const std::uint32_t mode) {
    close();
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC,
                 static_cast<mode_t>(mode));
    if (fd_ < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "open result ring for writing");
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      const auto error = errno;
      close();
      throw std::system_error(error, std::generic_category(),
                              "another depthd result-ring writer is active");
    }
    if (::fchmod(fd_, static_cast<mode_t>(mode)) != 0) {
      const auto error = errno;
      close();
      throw std::system_error(error, std::generic_category(),
                              "set result-ring permissions");
    }
    if (::ftruncate(fd_, static_cast<off_t>(ipc::kResultRingFileBytes)) != 0) {
      const auto error = errno;
      close();
      throw std::system_error(error, std::generic_category(),
                              "resize result ring");
    }
    map(PROT_READ | PROT_WRITE);
  }

  bool open_reader(const std::string& path, std::string& error) {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
      error = errno == ENOENT ? "result ring is missing"
                              : std::generic_category().message(errno);
      return false;
    }
    struct stat state {};
    if (::fstat(fd_, &state) != 0 ||
        static_cast<std::uint64_t>(state.st_size) !=
            ipc::kResultRingFileBytes) {
      error = "result ring has an incompatible file size";
      close();
      return false;
    }
    device_ = static_cast<std::uint64_t>(state.st_dev);
    inode_ = static_cast<std::uint64_t>(state.st_ino);
    map(PROT_READ);
    return true;
  }

  bool still_current(const std::string& path) const noexcept {
    if (fd_ < 0 || mapping_ == nullptr) {
      return false;
    }
    struct stat state {};
    return ::stat(path.c_str(), &state) == 0 &&
           static_cast<std::uint64_t>(state.st_dev) == device_ &&
           static_cast<std::uint64_t>(state.st_ino) == inode_ &&
           static_cast<std::uint64_t>(state.st_size) ==
               ipc::kResultRingFileBytes;
  }

  std::byte* bytes() noexcept {
    return static_cast<std::byte*>(mapping_);
  }
  const std::byte* bytes() const noexcept {
    return static_cast<const std::byte*>(mapping_);
  }
  bool open() const noexcept { return mapping_ != nullptr; }

 private:
  void map(const int protection) {
    mapping_size_ = static_cast<std::size_t>(ipc::kResultRingFileBytes);
    mapping_ = ::mmap(nullptr, mapping_size_, protection, MAP_SHARED, fd_, 0);
    if (mapping_ == MAP_FAILED) {
      mapping_ = nullptr;
      const auto error = errno;
      close();
      throw std::system_error(error, std::generic_category(),
                              "mmap result ring");
    }
    struct stat state {};
    if (::fstat(fd_, &state) == 0) {
      device_ = static_cast<std::uint64_t>(state.st_dev);
      inode_ = static_cast<std::uint64_t>(state.st_ino);
    }
  }

  int fd_{-1};
  void* mapping_{};
  std::size_t mapping_size_{};
  std::uint64_t device_{};
  std::uint64_t inode_{};
};
#endif

}  // namespace

ipc::CameraModel make_rectified_left_camera_model() noexcept {
  ipc::CameraModel model;
  model.distortion_count = 5;
  model.k = {493.542046188, 0.0, 348.240810394,
             0.0, 493.542046188, 266.509811401,
             0.0, 0.0, 1.0};
  model.r = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  model.p = {493.542046188, 0.0, 348.240810394, 0.0,
             0.0, 493.542046188, 266.509811401, 0.0,
             0.0, 0.0, 1.0, 0.0};
  return model;
}

ipc::CameraModel make_rectified_right_camera_model() noexcept {
  auto model = make_rectified_left_camera_model();
  model.p[3] = -49.3542046188;
  return model;
}

class ResultRingWriter::Impl {
 public:
  explicit Impl(ResultRingConfig config) : config_(std::move(config)) {
#if defined(__linux__) || defined(__APPLE__)
    if (config_.left_camera.p[0] == 0.0) {
      config_.left_camera = make_rectified_left_camera_model();
    }
    if (config_.right_camera.p[0] == 0.0) {
      config_.right_camera = make_rectified_right_camera_model();
    }
    if (config_.file_mode > 0777U) {
      throw std::invalid_argument("result-ring file mode is invalid");
    }
    validate_frame_id(config_.left_optical_frame);
    validate_frame_id(config_.right_optical_frame);
    validate_frame_id(config_.obstacle_frame);
    file_.open_writer(config_.path, config_.file_mode);
    std::memset(file_.bytes(), 0,
                static_cast<std::size_t>(ipc::kResultRingFileBytes));
    instance_id_ = make_instance_id();
    auto header = ipc::ResultRingHeader{};
    header.magic = 0U;
    header.instance_id = instance_id_;
    header.created_realtime_ns = realtime_now_ns();
    header.writer_pid = static_cast<std::uint64_t>(::getpid());
    std::memcpy(file_.bytes(), &header, sizeof(header));
    store_release(&ring_header()->magic, ipc::kResultRingMagic);
#else
    (void)config_;
    throw std::runtime_error(
        "the result-ring mmap writer is supported only on POSIX hosts");
#endif
  }

  std::uint64_t publish(const ResultSnapshotView& snapshot) {
#if defined(__linux__) || defined(__APPLE__)
    validate_snapshot(snapshot);
    const auto sequence = ++last_sequence_;
    const auto slot_index = sequence % ipc::kResultRingSlotCount;
    auto* slot = slot_header(slot_index);
    const auto odd_guard = sequence * 2U + 1U;
    const auto even_guard = sequence * 2U;
    store_release(&slot->begin_guard, odd_guard);
    store_release(&slot->end_guard, odd_guard);

    ipc::ResultSlotHeader metadata;
    metadata.begin_guard = odd_guard;
    metadata.end_guard = odd_guard;
    metadata.sequence = sequence;
    metadata.instance_id = instance_id_;
    metadata.capture_realtime_ns = snapshot.capture_realtime_ns;
    metadata.ready_realtime_ns = snapshot.ready_realtime_ns;
    metadata.calibration_id = config_.calibration_id;
    metadata.health = snapshot.health;
    metadata.product_flags = static_cast<std::uint32_t>(
        ipc::ProductFlags::kDisparity | ipc::ProductFlags::kLeftCameraInfo |
        ipc::ProductFlags::kRightCameraInfo |
        ipc::ProductFlags::kObstacleCloud);
    if (!snapshot.left_rectified_mono.empty()) {
      metadata.product_flags |= static_cast<std::uint32_t>(
          ipc::ProductFlags::kLeftRectifiedMono);
    }
    if (!snapshot.left_rectified_jpeg.empty()) {
      metadata.product_flags |= static_cast<std::uint32_t>(
          ipc::ProductFlags::kLeftRectifiedJpeg);
    }
    metadata.disparity_bytes = ipc::kDisparityCapacityBytes;
    metadata.mono_bytes = static_cast<std::uint32_t>(
        snapshot.left_rectified_mono.size());
    metadata.jpeg_bytes = static_cast<std::uint32_t>(
        snapshot.left_rectified_jpeg.size());
    metadata.obstacle_point_count = static_cast<std::uint32_t>(
        snapshot.obstacle_points.size());
    metadata.left_camera = config_.left_camera;
    metadata.right_camera = config_.right_camera;
    copy_string(metadata.left_optical_frame, config_.left_optical_frame);
    copy_string(metadata.right_optical_frame, config_.right_optical_frame);
    copy_string(metadata.obstacle_frame, config_.obstacle_frame);
    metadata.sampled_pixels = snapshot.obstacle_stats.sampled_pixels;
    metadata.accepted_raw_points =
        snapshot.obstacle_stats.accepted_raw_points;
    metadata.accumulated_voxels =
        snapshot.obstacle_stats.accumulated_voxels;
    metadata.supported_voxels = snapshot.obstacle_stats.supported_voxels;
    metadata.accepted_clusters = snapshot.obstacle_stats.accepted_clusters;
    metadata.voxel_capacity_overflow =
        snapshot.obstacle_stats.voxel_capacity_overflow;

    // Preserve the two live guards while replacing all other metadata.
    std::memcpy(reinterpret_cast<std::byte*>(slot) +
                    offsetof(ipc::ResultSlotHeader, sequence),
                reinterpret_cast<const std::byte*>(&metadata) +
                    offsetof(ipc::ResultSlotHeader, sequence),
                sizeof(metadata) -
                    offsetof(ipc::ResultSlotHeader, sequence));
    auto* slot_bytes = reinterpret_cast<std::byte*>(slot);
    std::memcpy(slot_bytes + ipc::kDisparityOffset,
                snapshot.disparity.data(), ipc::kDisparityCapacityBytes);
    if (!snapshot.left_rectified_mono.empty()) {
      std::memcpy(slot_bytes + ipc::kMonoOffset,
                  snapshot.left_rectified_mono.data(),
                  snapshot.left_rectified_mono.size());
    }
    if (!snapshot.left_rectified_jpeg.empty()) {
      std::memcpy(slot_bytes + ipc::kJpegOffset,
                  snapshot.left_rectified_jpeg.data(),
                  snapshot.left_rectified_jpeg.size());
    }
    if (!snapshot.obstacle_points.empty()) {
      static_assert(sizeof(Point3f) == 3U * sizeof(float));
      std::memcpy(slot_bytes + ipc::kObstacleOffset,
                  snapshot.obstacle_points.data(),
                  snapshot.obstacle_points.size_bytes());
    }

    store_release(&slot->end_guard, even_guard);
    store_release(&slot->begin_guard, even_guard);
    store_relaxed(&ring_header()->latest_slot, slot_index);
    store_release(&ring_header()->latest_sequence, sequence);
    return sequence;
#else
    (void)snapshot;
    throw std::runtime_error("result-ring writer is unavailable");
#endif
  }

  std::uint64_t instance_id() const noexcept { return instance_id_; }
  std::uint64_t last_sequence() const noexcept { return last_sequence_; }

 private:
#if defined(__linux__) || defined(__APPLE__)
  ipc::ResultRingHeader* ring_header() noexcept {
    return reinterpret_cast<ipc::ResultRingHeader*>(file_.bytes());
  }
  ipc::ResultSlotHeader* slot_header(const std::uint64_t index) noexcept {
    return reinterpret_cast<ipc::ResultSlotHeader*>(
        file_.bytes() + ipc::kResultRingHeaderBytes +
        index * ipc::kResultSlotStrideBytes);
  }
  MappedFile file_;
#endif
  ResultRingConfig config_;
  std::uint64_t instance_id_{};
  std::uint64_t last_sequence_{};
};

ResultRingWriter::ResultRingWriter(ResultRingConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
ResultRingWriter::~ResultRingWriter() = default;
ResultRingWriter::ResultRingWriter(ResultRingWriter&&) noexcept = default;
ResultRingWriter& ResultRingWriter::operator=(ResultRingWriter&&) noexcept =
    default;
std::uint64_t ResultRingWriter::instance_id() const noexcept {
  return impl_->instance_id();
}
std::uint64_t ResultRingWriter::last_sequence() const noexcept {
  return impl_->last_sequence();
}
std::uint64_t ResultRingWriter::publish(const ResultSnapshotView& snapshot) {
  return impl_->publish(snapshot);
}

class ResultRingReader::Impl {
 public:
  explicit Impl(std::string path) : path_(std::move(path)) {}

  ResultRingReadResult read_latest(ResultSnapshot& snapshot) {
#if defined(__linux__) || defined(__APPLE__)
    if (!file_.still_current(path_)) {
      std::string error;
      if (!file_.open_reader(path_, error)) {
        if (error != "result ring is missing") {
          ++schema_mismatches_;
        }
        return {error == "result ring is missing"
                    ? ResultRingReadStatus::kMissing
                    : ResultRingReadStatus::kSchemaMismatch,
                std::move(error), 0, dropped_sequences_, torn_reads_,
                schema_mismatches_};
      }
    }
    ipc::ResultRingHeader ring;
    std::memcpy(&ring, file_.bytes(), sizeof(ring));
    if (!valid_ring_header(ring)) {
      ++schema_mismatches_;
      return {ResultRingReadStatus::kSchemaMismatch,
              "result ring schema/layout mismatch", 0, dropped_sequences_,
              torn_reads_, schema_mismatches_};
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
      const auto* live_ring =
          reinterpret_cast<const ipc::ResultRingHeader*>(file_.bytes());
      const auto sequence = load_acquire(&live_ring->latest_sequence);
      const auto slot_index = load_acquire(&live_ring->latest_slot);
      if (sequence == 0U) {
        return {ResultRingReadStatus::kUnavailable,
                "result ring has no completed snapshot", 0,
                dropped_sequences_, torn_reads_, schema_mismatches_};
      }
      if (slot_index >= ipc::kResultRingSlotCount) {
        ++schema_mismatches_;
        return {ResultRingReadStatus::kSchemaMismatch,
                "result ring latest slot is out of range", sequence,
                dropped_sequences_, torn_reads_, schema_mismatches_};
      }
      if (last_instance_id_ == ring.instance_id &&
          last_sequence_ == sequence) {
        return {ResultRingReadStatus::kStale, "no newer result snapshot",
                sequence, dropped_sequences_, torn_reads_,
                schema_mismatches_};
      }

      const auto* slot_bytes =
          file_.bytes() + ipc::kResultRingHeaderBytes +
          slot_index * ipc::kResultSlotStrideBytes;
      const auto* live_slot =
          reinterpret_cast<const ipc::ResultSlotHeader*>(slot_bytes);
      const auto first_guard = load_acquire(&live_slot->begin_guard);
      if (first_guard == 0U || (first_guard & 1U) != 0U) {
        ++torn_reads_;
        continue;
      }
      ipc::ResultSlotHeader slot;
      std::memcpy(&slot, live_slot, sizeof(slot));
      if (slot.sequence != sequence || slot.instance_id != ring.instance_id ||
          slot.begin_guard != first_guard || !valid_slot_header(slot)) {
        ++torn_reads_;
        continue;
      }

      snapshot.header = slot;
      snapshot.disparity.resize(kExpectedDisparityElements);
      std::memcpy(snapshot.disparity.data(),
                  slot_bytes + ipc::kDisparityOffset,
                  ipc::kDisparityCapacityBytes);
      snapshot.left_rectified_mono.resize(slot.mono_bytes);
      if (slot.mono_bytes > 0U) {
        std::memcpy(snapshot.left_rectified_mono.data(),
                    slot_bytes + ipc::kMonoOffset, slot.mono_bytes);
      }
      snapshot.left_rectified_jpeg.resize(slot.jpeg_bytes);
      if (slot.jpeg_bytes > 0U) {
        std::memcpy(snapshot.left_rectified_jpeg.data(),
                    slot_bytes + ipc::kJpegOffset, slot.jpeg_bytes);
      }
      snapshot.obstacle_points.resize(slot.obstacle_point_count);
      if (slot.obstacle_point_count > 0U) {
        std::memcpy(snapshot.obstacle_points.data(),
                    slot_bytes + ipc::kObstacleOffset,
                    snapshot.obstacle_points.size() * sizeof(Point3f));
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      const auto final_end = load_acquire(&live_slot->end_guard);
      const auto final_begin = load_acquire(&live_slot->begin_guard);
      const auto latest_after = load_acquire(&live_ring->latest_sequence);
      if (final_begin != first_guard || final_end != first_guard ||
          (final_begin & 1U) != 0U) {
        ++torn_reads_;
        continue;
      }
      if (latest_after != sequence && attempt < 2) {
        continue;
      }

      const bool restarted = last_instance_id_ != 0U &&
                             last_instance_id_ != ring.instance_id;
      if (!restarted && last_sequence_ != 0U && sequence > last_sequence_ + 1U) {
        dropped_sequences_ += sequence - last_sequence_ - 1U;
      }
      last_instance_id_ = ring.instance_id;
      last_sequence_ = sequence;
      return {restarted ? ResultRingReadStatus::kRestarted
                        : ResultRingReadStatus::kOk,
              restarted ? "depthd writer instance changed" : "ok", sequence,
              dropped_sequences_, torn_reads_, schema_mismatches_};
    }
    ++torn_reads_;
    return {ResultRingReadStatus::kTorn,
            "could not copy a stable result slot", 0, dropped_sequences_,
            torn_reads_, schema_mismatches_};
#else
    (void)snapshot;
    return {ResultRingReadStatus::kUnavailable,
            "the result-ring mmap reader is supported only on POSIX hosts"};
#endif
  }

 private:
  std::string path_;
#if defined(__linux__) || defined(__APPLE__)
  MappedFile file_;
#endif
  std::uint64_t last_instance_id_{};
  std::uint64_t last_sequence_{};
  std::uint64_t dropped_sequences_{};
  std::uint64_t torn_reads_{};
  std::uint64_t schema_mismatches_{};
};

ResultRingReader::ResultRingReader(std::string path)
    : impl_(std::make_unique<Impl>(std::move(path))) {}
ResultRingReader::~ResultRingReader() = default;
ResultRingReader::ResultRingReader(ResultRingReader&&) noexcept = default;
ResultRingReader& ResultRingReader::operator=(ResultRingReader&&) noexcept =
    default;
ResultRingReadResult ResultRingReader::read_latest(ResultSnapshot& snapshot) {
  return impl_->read_latest(snapshot);
}

const char* result_ring_read_status_name(
    const ResultRingReadStatus status) noexcept {
  switch (status) {
    case ResultRingReadStatus::kOk:
      return "ok";
    case ResultRingReadStatus::kMissing:
      return "missing";
    case ResultRingReadStatus::kSchemaMismatch:
      return "schema_mismatch";
    case ResultRingReadStatus::kTorn:
      return "torn";
    case ResultRingReadStatus::kStale:
      return "stale";
    case ResultRingReadStatus::kRestarted:
      return "restarted";
    case ResultRingReadStatus::kUnavailable:
      return "unavailable";
  }
  return "unknown";
}

}  // namespace depthd
