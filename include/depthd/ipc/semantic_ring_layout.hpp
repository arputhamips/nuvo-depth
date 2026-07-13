#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace depthd::ipc {

inline constexpr std::uint64_t kSemanticRingMagic = 0x314d455344505444ULL;
inline constexpr std::uint32_t kSemanticRingSchemaVersion = 1U;
inline constexpr std::uint32_t kSemanticLittleEndianTag = 0x01020304U;
inline constexpr std::uint32_t kSemanticRingHeaderBytes = 4096U;
inline constexpr std::uint32_t kSemanticSlotHeaderBytes = 4096U;
inline constexpr std::uint32_t kSemanticRingSlotCount = 4U;
inline constexpr std::uint32_t kSemanticObjectCapacity = 64U;
inline constexpr std::uint32_t kSemanticObjectRecordBytes = 256U;
inline constexpr std::uint32_t kSemanticPreviewCapacityBytes = 512U * 1024U;
inline constexpr std::uint32_t kSemanticObjectOffset = kSemanticSlotHeaderBytes;
inline constexpr std::uint32_t kSemanticObjectCapacityBytes =
    kSemanticObjectCapacity * kSemanticObjectRecordBytes;
inline constexpr std::uint32_t kSemanticPreviewOffset =
    kSemanticObjectOffset + kSemanticObjectCapacityBytes;
inline constexpr std::uint32_t kSemanticSlotStrideBytes =
    kSemanticPreviewOffset + kSemanticPreviewCapacityBytes;
inline constexpr std::uint64_t kSemanticRingFileBytes =
    kSemanticRingHeaderBytes +
    static_cast<std::uint64_t>(kSemanticRingSlotCount) *
        kSemanticSlotStrideBytes;

enum class SemanticProductFlags : std::uint32_t {
  kObjects = 1U << 0U,
  kHazard = 1U << 1U,
  kPreviewJpeg = 1U << 2U,
};

constexpr bool has_semantic_flag(const std::uint32_t flags,
                                 const SemanticProductFlags flag) noexcept {
  return (flags & static_cast<std::uint32_t>(flag)) != 0U;
}

struct SemanticRingHeaderCore {
  std::uint64_t magic{kSemanticRingMagic};
  std::uint32_t schema_version{kSemanticRingSchemaVersion};
  std::uint32_t endian_tag{kSemanticLittleEndianTag};
  std::uint32_t header_bytes{kSemanticRingHeaderBytes};
  std::uint32_t slot_header_bytes{kSemanticSlotHeaderBytes};
  std::uint32_t slot_count{kSemanticRingSlotCount};
  std::uint32_t slot_stride_bytes{kSemanticSlotStrideBytes};
  std::uint32_t object_record_bytes{kSemanticObjectRecordBytes};
  std::uint32_t object_capacity{kSemanticObjectCapacity};
  std::uint32_t object_offset{kSemanticObjectOffset};
  std::uint32_t preview_offset{kSemanticPreviewOffset};
  std::uint32_t preview_capacity_bytes{kSemanticPreviewCapacityBytes};
  std::uint32_t reserved_alignment{};
  std::uint64_t file_bytes{kSemanticRingFileBytes};
  std::uint64_t instance_id{};
  std::int64_t created_realtime_ns{};
  std::uint64_t writer_pid{};
  std::uint64_t latest_sequence{};
};

struct alignas(64) SemanticRingHeader {
  SemanticRingHeaderCore core{};
  std::array<std::byte,
             kSemanticRingHeaderBytes - sizeof(SemanticRingHeaderCore)>
      reserved{};
};

struct SemanticModelRecordCore {
  std::array<char, 48> model_name{};
  std::array<char, 32> model_version{};
  std::array<char, 65> model_sha256{};
  std::uint32_t health{};
  std::uint32_t reserved0{};
  std::uint64_t completed_runs{};
  std::uint64_t errors{};
  float latency_ms{};
  float inference_latency_ms{};
  float postprocess_latency_ms{};
};

struct alignas(64) SemanticModelRecord {
  SemanticModelRecordCore core{};
  std::array<std::byte, 256U - sizeof(SemanticModelRecordCore)> reserved{};
};

struct SemanticHazardRecordCore {
  std::uint32_t active{};
  std::uint32_t perception_stale{};
  std::uint32_t fail_closed_required{};
  std::uint32_t source_observation_id{};
  std::uint16_t semantic_class{};
  std::uint16_t reserved0{};
  float range_m{};
  float bearing_rad{};
  std::array<char, 64> reason{};
};

struct alignas(64) SemanticHazardRecord {
  SemanticHazardRecordCore core{};
  std::array<std::byte, 128U - sizeof(SemanticHazardRecordCore)> reserved{};
};

struct SemanticSlotHeaderCore {
  std::uint64_t begin_guard{};
  std::uint64_t end_guard{};
  std::uint64_t sequence{};
  std::uint64_t instance_id{};
  std::uint64_t source_sequence{};
  std::int64_t capture_realtime_ns{};
  std::int64_t ready_realtime_ns{};
  std::uint32_t health{};
  std::uint32_t product_flags{};
  std::uint32_t object_count{};
  std::uint32_t instance_count{};
  std::uint32_t dense_region_count{};
  std::uint32_t preview_jpeg_bytes{};
  std::uint32_t model_count{};
  std::uint32_t reserved0{};
  std::uint64_t submitted_frames{};
  std::uint64_t replaced_pending_frames{};
  std::uint64_t processed_frames{};
  std::uint64_t published_results{};
  std::uint64_t worker_errors{};
  float frame_latency_ms{};
  float max_frame_latency_ms{};
  std::array<char, 64> target_frame{};
  SemanticHazardRecord hazard{};
  std::array<SemanticModelRecord, 4> models{};
};

struct alignas(64) SemanticSlotHeader {
  SemanticSlotHeaderCore core{};
  std::array<std::byte,
             kSemanticSlotHeaderBytes - sizeof(SemanticSlotHeaderCore)>
      reserved{};
};

struct SemanticObjectRecordCore {
  std::uint8_t source_kind{};
  std::uint8_t range_valid{};
  std::uint16_t semantic_class{};
  std::uint32_t observation_id{};
  std::uint32_t model_class_id{};
  std::uint32_t pixel_count{};
  std::uint32_t flags{};
  float confidence{};
  float x_min{};
  float y_min{};
  float x_max{};
  float y_max{};
  float point_x{};
  float point_y{};
  float point_z{};
  float range_m{};
  float bearing_rad{};
  float range_stddev_m{};
  float valid_fraction{};
  std::uint32_t valid_samples{};
  std::array<char, 48> model_name{};
  std::array<char, 32> model_label{};
};

struct alignas(64) SemanticObjectRecord {
  SemanticObjectRecordCore core{};
  std::array<std::byte,
             kSemanticObjectRecordBytes - sizeof(SemanticObjectRecordCore)>
      reserved{};
};

static_assert(sizeof(SemanticRingHeader) == kSemanticRingHeaderBytes);
static_assert(sizeof(SemanticSlotHeader) == kSemanticSlotHeaderBytes);
static_assert(sizeof(SemanticObjectRecord) == kSemanticObjectRecordBytes);
static_assert(sizeof(SemanticModelRecord) == 256U);
static_assert(sizeof(SemanticHazardRecord) == 128U);
static_assert(std::is_trivially_copyable_v<SemanticRingHeader>);
static_assert(std::is_trivially_copyable_v<SemanticSlotHeader>);
static_assert(std::is_trivially_copyable_v<SemanticObjectRecord>);
static_assert(kSemanticSlotStrideBytes % 4096U == 0U);

}  // namespace depthd::ipc
