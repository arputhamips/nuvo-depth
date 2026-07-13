#include "depthd/semantic_ring.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
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

std::int64_t realtime_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::uint64_t make_instance_id() {
  std::random_device device;
  const auto random = (static_cast<std::uint64_t>(device()) << 32U) |
                      static_cast<std::uint64_t>(device());
  const auto value = random ^ static_cast<std::uint64_t>(realtime_now_ns());
  return value == 0U ? 1U : value;
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

template <std::size_t Size>
void copy_checked(std::array<char, Size>& destination,
                  const std::string& source, const char* field) {
  if (source.size() >= Size) {
    throw std::invalid_argument(std::string("semantic-ring ") + field +
                                " is too long");
  }
  destination.fill('\0');
  std::memcpy(destination.data(), source.data(), source.size());
}

bool terminated(const auto& value) noexcept {
  return std::memchr(value.data(), '\0', value.size()) != nullptr;
}

bool valid_header(const ipc::SemanticRingHeader& header) noexcept {
  const auto& core = header.core;
  return core.magic == ipc::kSemanticRingMagic &&
         core.schema_version == ipc::kSemanticRingSchemaVersion &&
         core.endian_tag == ipc::kSemanticLittleEndianTag &&
         core.header_bytes == ipc::kSemanticRingHeaderBytes &&
         core.slot_header_bytes == ipc::kSemanticSlotHeaderBytes &&
         core.slot_count == ipc::kSemanticRingSlotCount &&
         core.slot_stride_bytes == ipc::kSemanticSlotStrideBytes &&
         core.object_record_bytes == ipc::kSemanticObjectRecordBytes &&
         core.object_capacity == ipc::kSemanticObjectCapacity &&
         core.object_offset == ipc::kSemanticObjectOffset &&
         core.preview_offset == ipc::kSemanticPreviewOffset &&
         core.preview_capacity_bytes == ipc::kSemanticPreviewCapacityBytes &&
         core.file_bytes == ipc::kSemanticRingFileBytes &&
         core.instance_id != 0U;
}

bool valid_slot(const ipc::SemanticSlotHeader& header) noexcept {
  const auto& core = header.core;
  if (core.sequence == 0U || core.instance_id == 0U ||
      core.source_sequence == 0U || core.capture_realtime_ns <= 0 ||
      core.ready_realtime_ns < core.capture_realtime_ns ||
      core.object_count > ipc::kSemanticObjectCapacity ||
      core.instance_count + core.dense_region_count != core.object_count ||
      core.preview_jpeg_bytes > ipc::kSemanticPreviewCapacityBytes ||
      core.model_count > core.models.size() || !terminated(core.target_frame)) {
    return false;
  }
  const bool preview_flag = ipc::has_semantic_flag(
      core.product_flags, ipc::SemanticProductFlags::kPreviewJpeg);
  return preview_flag == (core.preview_jpeg_bytes > 0U);
}

void validate_snapshot(const SemanticRingSnapshotView& snapshot) {
  if (snapshot.result == nullptr || snapshot.stats == nullptr) {
    throw std::invalid_argument("semantic-ring snapshot pointers are null");
  }
  const auto& result = *snapshot.result;
  if (result.source_sequence == 0U || result.capture_realtime_ns <= 0 ||
      result.ready_realtime_ns < result.capture_realtime_ns) {
    throw std::invalid_argument("semantic-ring timestamps are invalid");
  }
  if (result.objects.size() > ipc::kSemanticObjectCapacity) {
    throw std::invalid_argument("semantic-ring object capacity exceeded");
  }
  if (result.models.size() > 4U) {
    throw std::invalid_argument("semantic-ring model capacity exceeded");
  }
  if (snapshot.preview_jpeg.size() > ipc::kSemanticPreviewCapacityBytes) {
    throw std::invalid_argument("semantic-ring preview capacity exceeded");
  }
}

#if defined(__linux__) || defined(__APPLE__)
class MappedFile {
 public:
  ~MappedFile() { close(); }
  MappedFile() = default;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  void close() noexcept {
    if (mapping_ != nullptr) {
      ::munmap(mapping_, static_cast<std::size_t>(ipc::kSemanticRingFileBytes));
      mapping_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    device_ = 0U;
    inode_ = 0U;
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
                              "open semantic ring for writing");
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      const auto error = errno;
      close();
      throw std::system_error(error, std::generic_category(),
                              "another semantic-ring writer is active");
    }
    if (::fchmod(fd_, static_cast<mode_t>(mode)) != 0 ||
        ::ftruncate(fd_, static_cast<off_t>(ipc::kSemanticRingFileBytes)) != 0) {
      const auto error = errno;
      close();
      throw std::system_error(error, std::generic_category(),
                              "prepare semantic ring");
    }
    map(PROT_READ | PROT_WRITE);
  }

  bool open_reader(const std::string& path, std::string& error) {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
      error = errno == ENOENT ? "semantic ring is missing"
                              : std::generic_category().message(errno);
      return false;
    }
    struct stat state {};
    if (::fstat(fd_, &state) != 0 ||
        static_cast<std::uint64_t>(state.st_size) !=
            ipc::kSemanticRingFileBytes) {
      error = "semantic ring has an incompatible file size";
      close();
      return false;
    }
    device_ = static_cast<std::uint64_t>(state.st_dev);
    inode_ = static_cast<std::uint64_t>(state.st_ino);
    map(PROT_READ);
    return true;
  }

  bool current(const std::string& path) const noexcept {
    struct stat state {};
    return mapping_ != nullptr && ::stat(path.c_str(), &state) == 0 &&
           static_cast<std::uint64_t>(state.st_dev) == device_ &&
           static_cast<std::uint64_t>(state.st_ino) == inode_ &&
           static_cast<std::uint64_t>(state.st_size) ==
               ipc::kSemanticRingFileBytes;
  }

  std::byte* bytes() noexcept { return static_cast<std::byte*>(mapping_); }
  const std::byte* bytes() const noexcept {
    return static_cast<const std::byte*>(mapping_);
  }
  bool open() const noexcept { return mapping_ != nullptr; }

 private:
  void map(const int protection) {
    mapping_ = ::mmap(nullptr, static_cast<std::size_t>(
                                  ipc::kSemanticRingFileBytes),
                      protection, MAP_SHARED, fd_, 0);
    if (mapping_ == MAP_FAILED) {
      mapping_ = nullptr;
      const auto error = errno;
      close();
      throw std::system_error(error, std::generic_category(),
                              "mmap semantic ring");
    }
    struct stat state {};
    if (::fstat(fd_, &state) == 0) {
      device_ = static_cast<std::uint64_t>(state.st_dev);
      inode_ = static_cast<std::uint64_t>(state.st_ino);
    }
  }

  int fd_{-1};
  void* mapping_{};
  std::uint64_t device_{};
  std::uint64_t inode_{};
};
#endif

}  // namespace

class SemanticRingWriter::Impl {
 public:
  explicit Impl(SemanticRingConfig config) : config_(std::move(config)) {
#if defined(__linux__) || defined(__APPLE__)
    if (config_.file_mode > 0777U || config_.target_frame.empty() ||
        config_.target_frame.size() >= 64U) {
      throw std::invalid_argument("invalid semantic-ring configuration");
    }
    file_.open_writer(config_.path, config_.file_mode);
    std::memset(file_.bytes(), 0,
                static_cast<std::size_t>(ipc::kSemanticRingFileBytes));
    instance_id_ = make_instance_id();
    ipc::SemanticRingHeader header;
    header.core.magic = 0U;
    header.core.instance_id = instance_id_;
    header.core.created_realtime_ns = realtime_now_ns();
    header.core.writer_pid = static_cast<std::uint64_t>(::getpid());
    std::memcpy(file_.bytes(), &header, sizeof(header));
    store_release(&ring_header()->core.magic, ipc::kSemanticRingMagic);
#else
    (void)config_;
    throw std::runtime_error(
        "semantic mmap ring is supported only on POSIX hosts");
#endif
  }

  std::uint64_t publish(const SemanticRingSnapshotView& snapshot) {
#if defined(__linux__) || defined(__APPLE__)
    validate_snapshot(snapshot);
    const auto sequence = ++last_sequence_;
    const auto slot_index = sequence % ipc::kSemanticRingSlotCount;
    auto* slot = slot_header(slot_index);
    const auto odd = sequence * 2U + 1U;
    const auto even = sequence * 2U;
    store_release(&slot->core.begin_guard, odd);
    store_release(&slot->core.end_guard, odd);

    ipc::SemanticSlotHeader metadata;
    auto& core = metadata.core;
    core.begin_guard = odd;
    core.end_guard = odd;
    core.sequence = sequence;
    core.instance_id = instance_id_;
    core.source_sequence = snapshot.result->source_sequence;
    core.capture_realtime_ns = snapshot.result->capture_realtime_ns;
    core.ready_realtime_ns = snapshot.result->ready_realtime_ns;
    core.health = static_cast<std::uint32_t>(snapshot.result->health);
    core.product_flags = static_cast<std::uint32_t>(
        ipc::SemanticProductFlags::kObjects) |
        static_cast<std::uint32_t>(ipc::SemanticProductFlags::kHazard);
    if (!snapshot.preview_jpeg.empty()) {
      core.product_flags |= static_cast<std::uint32_t>(
          ipc::SemanticProductFlags::kPreviewJpeg);
    }
    core.object_count = static_cast<std::uint32_t>(
        snapshot.result->objects.size());
    core.preview_jpeg_bytes = static_cast<std::uint32_t>(
        snapshot.preview_jpeg.size());
    for (const auto& object : snapshot.result->objects) {
      if (object.observation.source_kind == SemanticSourceKind::kInstance) {
        ++core.instance_count;
      } else {
        ++core.dense_region_count;
      }
    }
    core.model_count = static_cast<std::uint32_t>(snapshot.result->models.size());
    core.submitted_frames = snapshot.stats->submitted_frames;
    core.replaced_pending_frames = snapshot.stats->replaced_pending_frames;
    core.processed_frames = snapshot.stats->processed_frames;
    core.published_results = snapshot.stats->published_results;
    core.worker_errors = snapshot.stats->worker_errors;
    core.frame_latency_ms = static_cast<float>(
        snapshot.stats->last_frame_latency_ms);
    core.max_frame_latency_ms = static_cast<float>(
        snapshot.stats->max_frame_latency_ms);
    copy_checked(core.target_frame, config_.target_frame, "target frame");
    core.hazard.core.active = snapshot.result->hazard.active ? 1U : 0U;
    core.hazard.core.perception_stale =
        snapshot.result->hazard.perception_stale ? 1U : 0U;
    core.hazard.core.fail_closed_required =
        snapshot.result->hazard.fail_closed_required ? 1U : 0U;
    core.hazard.core.source_observation_id =
        snapshot.result->hazard.source_observation_id;
    core.hazard.core.semantic_class = static_cast<std::uint16_t>(
        snapshot.result->hazard.semantic_class);
    core.hazard.core.range_m = snapshot.result->hazard.range_m;
    core.hazard.core.bearing_rad = snapshot.result->hazard.bearing_rad;
    copy_checked(core.hazard.core.reason, snapshot.result->hazard.reason,
                 "hazard reason");
    for (std::size_t index = 0; index < snapshot.result->models.size(); ++index) {
      const auto& source = snapshot.result->models[index];
      auto& target = core.models[index].core;
      copy_checked(target.model_name, source.model_name, "model name");
      copy_checked(target.model_version, source.model_version, "model version");
      copy_checked(target.model_sha256, source.model_sha256, "model SHA-256");
      target.health = static_cast<std::uint32_t>(source.health);
      target.completed_runs = source.completed_runs;
      target.errors = source.errors;
      target.latency_ms = static_cast<float>(source.latency_ms);
      target.inference_latency_ms = static_cast<float>(
          source.inference_latency_ms);
      target.postprocess_latency_ms = static_cast<float>(
          source.postprocess_latency_ms);
    }
    std::memcpy(slot, &metadata, sizeof(metadata));

    auto* slot_bytes = reinterpret_cast<std::byte*>(slot);
    auto* records = reinterpret_cast<ipc::SemanticObjectRecord*>(
        slot_bytes + ipc::kSemanticObjectOffset);
    std::fill_n(records, ipc::kSemanticObjectCapacity,
                ipc::SemanticObjectRecord{});
    for (std::size_t index = 0; index < snapshot.result->objects.size(); ++index) {
      const auto& source = snapshot.result->objects[index];
      auto& target = records[index].core;
      target.source_kind = static_cast<std::uint8_t>(
          source.observation.source_kind);
      target.range_valid = source.range_valid ? 1U : 0U;
      target.semantic_class = static_cast<std::uint16_t>(
          source.observation.semantic_class);
      target.observation_id = source.observation.observation_id;
      target.model_class_id = source.observation.model_class_id;
      target.pixel_count = source.observation.pixel_count;
      target.confidence = source.observation.confidence;
      target.x_min = source.observation.box.x_min;
      target.y_min = source.observation.box.y_min;
      target.x_max = source.observation.box.x_max;
      target.y_max = source.observation.box.y_max;
      target.point_x = source.point.x;
      target.point_y = source.point.y;
      target.point_z = source.point.z;
      target.range_m = source.range_m;
      target.bearing_rad = source.bearing_rad;
      target.range_stddev_m = source.range_stddev_m;
      target.valid_fraction = source.valid_fraction;
      target.valid_samples = source.valid_samples;
      copy_checked(target.model_name, source.observation.model_name,
                   "object model name");
      copy_checked(target.model_label, source.observation.model_label,
                   "object model label");
    }
    if (!snapshot.preview_jpeg.empty()) {
      std::memcpy(slot_bytes + ipc::kSemanticPreviewOffset,
                  snapshot.preview_jpeg.data(), snapshot.preview_jpeg.size());
    }
    store_release(&slot->core.end_guard, even);
    store_release(&slot->core.begin_guard, even);
    store_release(&ring_header()->core.latest_sequence, sequence);
    return sequence;
#else
    (void)snapshot;
    throw std::runtime_error("semantic mmap ring unavailable");
#endif
  }

  std::uint64_t instance_id() const noexcept { return instance_id_; }

 private:
#if defined(__linux__) || defined(__APPLE__)
  ipc::SemanticRingHeader* ring_header() noexcept {
    return reinterpret_cast<ipc::SemanticRingHeader*>(file_.bytes());
  }
  ipc::SemanticSlotHeader* slot_header(const std::uint64_t index) noexcept {
    return reinterpret_cast<ipc::SemanticSlotHeader*>(
        file_.bytes() + ipc::kSemanticRingHeaderBytes +
        index * ipc::kSemanticSlotStrideBytes);
  }
  MappedFile file_{};
#endif
  SemanticRingConfig config_{};
  std::uint64_t instance_id_{};
  std::uint64_t last_sequence_{};
};

SemanticRingWriter::SemanticRingWriter(SemanticRingConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
SemanticRingWriter::~SemanticRingWriter() = default;
SemanticRingWriter::SemanticRingWriter(SemanticRingWriter&&) noexcept = default;
SemanticRingWriter& SemanticRingWriter::operator=(SemanticRingWriter&&) noexcept =
    default;
std::uint64_t SemanticRingWriter::instance_id() const noexcept {
  return impl_->instance_id();
}
std::uint64_t SemanticRingWriter::publish(
    const SemanticRingSnapshotView& snapshot) {
  return impl_->publish(snapshot);
}

class SemanticRingReader::Impl {
 public:
  explicit Impl(std::string path) : path_(std::move(path)) {}

  SemanticRingReadResult read_latest(SemanticRingSnapshot& snapshot) {
#if defined(__linux__) || defined(__APPLE__)
    if (!file_.open() || !file_.current(path_)) {
      std::string error;
      if (!file_.open_reader(path_, error)) {
        return {error == "semantic ring is missing"
                    ? SemanticRingReadStatus::kMissing
                    : SemanticRingReadStatus::kSchemaMismatch,
                error, 0U, dropped_, torn_, schema_};
      }
    }
    ipc::SemanticRingHeader header;
    std::memcpy(&header, file_.bytes(), sizeof(header));
    if (!valid_header(header)) {
      ++schema_;
      return {SemanticRingReadStatus::kSchemaMismatch,
              "semantic ring schema mismatch", 0U, dropped_, torn_, schema_};
    }
    const auto sequence = load_acquire(
        &reinterpret_cast<const ipc::SemanticRingHeader*>(file_.bytes())
             ->core.latest_sequence);
    if (sequence == 0U) {
      return {SemanticRingReadStatus::kUnavailable,
              "semantic ring has no complete result", 0U, dropped_, torn_,
              schema_};
    }
    if (header.core.instance_id == last_instance_ && sequence == last_sequence_) {
      return {SemanticRingReadStatus::kStale, "semantic result is unchanged",
              sequence, dropped_, torn_, schema_};
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
      const auto* slot = reinterpret_cast<const ipc::SemanticSlotHeader*>(
          file_.bytes() + ipc::kSemanticRingHeaderBytes +
          (sequence % ipc::kSemanticRingSlotCount) *
              ipc::kSemanticSlotStrideBytes);
      const auto first = load_acquire(&slot->core.begin_guard);
      if ((first & 1U) != 0U || first != sequence * 2U) {
        ++torn_;
        continue;
      }
      ipc::SemanticSlotHeader metadata;
      std::memcpy(&metadata, slot, sizeof(metadata));
      if (metadata.core.sequence != sequence ||
          metadata.core.instance_id != header.core.instance_id ||
          !valid_slot(metadata)) {
        ++torn_;
        continue;
      }
      snapshot.header = metadata;
      snapshot.objects.resize(metadata.core.object_count);
      const auto* slot_bytes = reinterpret_cast<const std::byte*>(slot);
      std::memcpy(snapshot.objects.data(),
                  slot_bytes + ipc::kSemanticObjectOffset,
                  snapshot.objects.size() * sizeof(ipc::SemanticObjectRecord));
      snapshot.preview_jpeg.resize(metadata.core.preview_jpeg_bytes);
      std::memcpy(snapshot.preview_jpeg.data(),
                  slot_bytes + ipc::kSemanticPreviewOffset,
                  snapshot.preview_jpeg.size());
      std::atomic_thread_fence(std::memory_order_acquire);
      const auto final_begin = load_acquire(&slot->core.begin_guard);
      const auto final_end = load_acquire(&slot->core.end_guard);
      const auto latest_after = load_acquire(
          &reinterpret_cast<const ipc::SemanticRingHeader*>(file_.bytes())
               ->core.latest_sequence);
      if (final_begin != first || final_end != first ||
          latest_after != sequence) {
        ++torn_;
        continue;
      }
      const bool restarted = last_instance_ != 0U &&
                             last_instance_ != header.core.instance_id;
      if (!restarted && last_sequence_ != 0U && sequence > last_sequence_ + 1U) {
        dropped_ += sequence - last_sequence_ - 1U;
      }
      last_instance_ = header.core.instance_id;
      last_sequence_ = sequence;
      return {restarted ? SemanticRingReadStatus::kRestarted
                        : SemanticRingReadStatus::kOk,
              restarted ? "semantic writer instance changed" : "ok", sequence,
              dropped_, torn_, schema_};
    }
    return {SemanticRingReadStatus::kTorn,
            "could not copy a stable semantic slot", 0U, dropped_, torn_,
            schema_};
#else
    (void)snapshot;
    return {SemanticRingReadStatus::kUnavailable,
            "semantic mmap ring is supported only on POSIX hosts"};
#endif
  }

 private:
  std::string path_{};
#if defined(__linux__) || defined(__APPLE__)
  MappedFile file_{};
#endif
  std::uint64_t last_instance_{};
  std::uint64_t last_sequence_{};
  std::uint64_t dropped_{};
  std::uint64_t torn_{};
  std::uint64_t schema_{};
};

SemanticRingReader::SemanticRingReader(std::string path)
    : impl_(std::make_unique<Impl>(std::move(path))) {}
SemanticRingReader::~SemanticRingReader() = default;
SemanticRingReader::SemanticRingReader(SemanticRingReader&&) noexcept = default;
SemanticRingReader& SemanticRingReader::operator=(SemanticRingReader&&) noexcept =
    default;
SemanticRingReadResult SemanticRingReader::read_latest(
    SemanticRingSnapshot& snapshot) {
  return impl_->read_latest(snapshot);
}

const char* semantic_ring_read_status_name(
    const SemanticRingReadStatus status) noexcept {
  switch (status) {
    case SemanticRingReadStatus::kOk:
      return "ok";
    case SemanticRingReadStatus::kMissing:
      return "missing";
    case SemanticRingReadStatus::kSchemaMismatch:
      return "schema_mismatch";
    case SemanticRingReadStatus::kTorn:
      return "torn";
    case SemanticRingReadStatus::kStale:
      return "stale";
    case SemanticRingReadStatus::kRestarted:
      return "restarted";
    case SemanticRingReadStatus::kUnavailable:
      return "unavailable";
  }
  return "unknown";
}

}  // namespace depthd
